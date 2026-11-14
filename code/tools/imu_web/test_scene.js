import * as THREE from "three";
import {applyOrientation, armKinematics, createWorld, DIMENSIONS, poseAt, quaternionFromRows,
  RIGHT_GRIP, updateWorld} from "/scene.js";

export function runTests() {
  const passed = [];
  function check(condition, message) {
    if (!condition) throw new Error(message);
  }
  const close = (a,b) => Math.abs(a-b)<1e-8;
  const identity = [[1,0,0],[0,1,0],[0,0,1]];
  const quarterTurn = [[0,-1,0],[1,0,0],[0,0,1]];
  const world = createWorld();
  for (const [sensor, expected] of [["board",[8,5,2]],["wt",[26,8,6]]]) {
    const body = world[sensor].getObjectByName("reference-block");
    body.geometry.computeBoundingBox();
    const measured = body.geometry.boundingBox.getSize(new THREE.Vector3()).toArray();
    check(measured.every((value,index)=>close(value,expected[index])), `${sensor}: actual geometry dimensions`);
    check(DIMENSIONS[sensor].every((value,index)=>value===expected[index]), `${sensor}: dimensions metadata`);
  }
  passed.push("Real box dimensions are 8x5x2 and 26x8x6 cm");
  const forward = world.scene.getObjectByName("bike-forward");
  const initial = forward.quaternion.clone();
  const quaternion = quaternionFromRows(quarterTurn);
  applyOrientation(world.board, quaternion);
  world.scene.updateMatrixWorld(true);
  const attitude = world.board.getObjectByName("attitude");
  const heading = new THREE.Vector3(1,0,0).applyQuaternion(attitude.quaternion);
  check(heading.distanceTo(new THREE.Vector3(0,1,0))<1e-8, "Row-major matrix must rotate X into Y");
  const arrow = world.board.getObjectByName("sensor-axes").getObjectByName("X");
  const arrowDirection = new THREE.Vector3(0,1,0).applyQuaternion(arrow.getWorldQuaternion(new THREE.Quaternion()));
  check(arrowDirection.distanceTo(heading)<1e-8, "Device arrow must rotate with reference block");
  check(forward.quaternion.equals(initial), "Changing only the test device must not mutate cockpit reference");
  passed.push("Box and X arrow rotate 90 degrees together");
  applyOrientation(world.board,new THREE.Quaternion());
  const identityQuaternion = new THREE.Quaternion();
  const first = updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true});
  const initialCenter = first.kinematics.center.clone();
  const northQuaternion = world.north.quaternion.clone();
  const turning = updateWorld(world,{board:quaternion,wt:quaternion,paired:true});
  world.scene.updateMatrixWorld(true);
  check(turning.kinematics.center.distanceTo(initialCenter.applyQuaternion(quaternion))<1e-8,
    "Common turning must rotate the forearm position with the cockpit, not twice");
  check(world.board.getWorldQuaternion(new THREE.Quaternion()).angleTo(quaternion)<1e-7,
    "Computer follows cockpit turning exactly once");
  check(world.wt.getObjectByName("attitude").getWorldQuaternion(new THREE.Quaternion()).angleTo(quaternion)<1e-7,
    "Arm follows common turning exactly once");
  check(world.forward.getWorldQuaternion(new THREE.Quaternion()).angleTo(quaternion)<1e-7,
    "Bike forward arrow must follow cockpit");
  check(world.north.quaternion.equals(northQuaternion), "Horizontal north must remain world-fixed");
  check(close(turning.heading,270), "World counterclockwise +90 is compass heading 270");
  passed.push("Common turns rotate cockpit and arm once; north stays fixed");
  const clockwise = updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true},{initialHeading:90});
  check(close(clockwise.heading,90), "Manual heading 90 must point east");
  const bar = world.cockpit.getObjectByName("bar-center");
  check(close(-bar.position.x,8), "Computer center must be 8 cm ahead of handlebar");
  const leftHood = world.cockpit.getObjectByName("left-hood");
  const rightHood = world.cockpit.getObjectByName("right-hood");
  check(close(leftHood.position.y-rightHood.position.y,38), "Hood center-to-center width must be 38 cm");
  passed.push("Manual compass heading, 38 cm cockpit and 8 cm computer offset");
  const supported = armKinematics(identityQuaternion,quaternion,"hood");
  check(supported.hand.distanceTo(new THREE.Vector3(...RIGHT_GRIP))<1e-8,
    "Supported palm stays on right hood as elbow moves");
  const neutralFree = armKinematics(identityQuaternion,identityQuaternion,"free");
  const movedFree = armKinematics(identityQuaternion,quaternion,"free");
  check(movedFree.elbow.distanceTo(neutralFree.elbow)<1e-8, "Free mode elbow is the pivot");
  check(movedFree.wrist.distanceTo(neutralFree.wrist)>36, "Free wrist must move through full 26 cm lever arm");
  check(close(movedFree.wrist.distanceTo(movedFree.elbow),26), "Forearm length must be preserved");
  passed.push("Hood support and large-range wrist motion preserve limb length");
  const frames = [
    {time_ms:0,paired:true,board:identity,wt:identity},
    {time_ms:100,paired:true,board:identity,wt:quarterTurn},
  ];
  const midpoint = poseAt(frames,50);
  check(close(midpoint.angle,45), "Midpoint must be quaternion-interpolated 45 degrees");
  check(midpoint.index===0 && poseAt(frames,100).index===1, "Timeline boundary must select correct sample");
  passed.push("50 ms midpoint is 45 degrees and sample boundary is correct");
  const gap = poseAt([frames[0],{...frames[1],time_ms:1000}],500);
  check(gap.board===null && gap.wt===null && gap.angle===null, "Do not fabricate motion across long gaps");
  passed.push("Long gaps hide unavailable orientations");
  applyOrientation(world.wt,null);
  check(!world.wt.getObjectByName("attitude").visible, "Invalid device hides its block and arrows");
  check(world.wt.getObjectByName("unavailable").visible, "Invalid device retains labeled ghost mount");
  check(poseAt([{...frames[0],paired:false}],0).angle===null, "Unpaired sample must not have comparison angle");
  updateWorld(world,{board:null,wt:identityQuaternion,paired:false});
  check(!world.cockpit.visible && !world.wt.visible && world.north.visible,
    "Missing board must not fabricate a position or hide reference north");
  passed.push("Invalid sensor arrows hidden and unpaired angle unavailable");
  world.scene.traverse(object=>{
    object.geometry?.dispose();
    if (object.material) {
      object.material.map?.dispose();
      object.material.dispose();
    }
  });
  return {passed, count:passed.length};
}
