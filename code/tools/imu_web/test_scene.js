import * as THREE from "three";
import {applyOrientation, armKinematics, createWorld, DIMENSIONS, poseAt, quaternionFromRows,
  RIGHT_GRIP, updateWorld, wristAxisMapping, setTextVisibility} from "/scene.js";
import {setRiderDimensions, twoLinkJoint} from "/bike_model.js";

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
  const mount=wristAxisMapping(["-X","-Y","+Z"]);
  const mapped=updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true},{wristMount:mount});
  check(close(mapped.angle,180), "Axis remap changes neutral mounting and displayed comparison angle");
  check(mapped.kinematics.hand.distanceTo(new THREE.Vector3(...RIGHT_GRIP))<1e-8,
    "Remapping preserves hood support");
  const mappedTurn=updateWorld(world,{board:quaternion,wt:quaternion,paired:true},{wristMount:mount});
  check(mappedTurn.kinematics.wrist.distanceTo(mapped.kinematics.wrist.clone().applyQuaternion(quaternion))<1e-8,
    "Remapped mounting preserves common turning, without changing world turn axes");
  const cycle=wristAxisMapping(["+Y","+Z","+X"]);
  check(new THREE.Vector3(1,0,0).applyQuaternion(cycle).distanceTo(new THREE.Vector3(0,1,0))<1e-8,
    "Axis permutation maps model X to selected Y");
  for (const invalid of [["+X","+X","+Z"],["-X","+Y","+Z"],["bad","+Y","+Z"]]) {
    let rejected=false;
    try {wristAxisMapping(invalid);} catch {rejected=true;}
    check(rejected, "Duplicate, reflected and malformed mappings must be rejected");
  }
  passed.push("Wrist mapping changes pose, rejects reflections and preserves common turns");
  setTextVisibility(world,false);
  let hidden=0;
  world.scene.traverse(object=>{if (object.isSprite) {check(!object.visible,"All 3D labels hide");hidden++;}});
  check(hidden>0 && world.north.visible && world.forward.visible,
    "Hiding text must preserve both direction arrows");
  setTextVisibility(world,true);
  world.scene.traverse(object=>{if (object.isSprite) check(object.visible,"3D text can be restored");});
  passed.push("All scene text toggles without removing arrows");
  updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true});
  world.scene.updateMatrixWorld(true);
  const lengthOf=(mesh,axis="y")=>{
    mesh.geometry.computeBoundingBox();
    const box=mesh.geometry.boundingBox;
    const start=new THREE.Vector3(), end=new THREE.Vector3();
    start[axis]=box.min[axis]; end[axis]=box.max[axis];
    return mesh.localToWorld(start).distanceTo(mesh.localToWorld(end));
  };
  const bike=world.bike;
  const frontWheel=bike.root.getObjectByName("front-wheel");
  const rearWheel=bike.root.getObjectByName("rear-wheel");
  check(close(frontWheel.position.distanceTo(rearWheel.position),96.6), "Actual axle spacing must be 966 mm");
  for (const [name,length] of [["stem",11],["crank--1",17],["crank-1",17],
    ["head-tube",11],["seat-tube",46],["chainstay-1",41]]) {
    check(close(lengthOf(bike.root.getObjectByName(name)),length), `${name}: actual physical centerline length`);
  }
  const bottom=bike.root.getObjectByName("bb").position;
  const top=bike.root.getObjectByName("headTop").position;
  check(close(top.x-bottom.x,37.5) && close(top.z-bottom.z,50.7), "Size 49 reach/stack must be 375/507 mm");
  const tire=frontWheel.getObjectByName("tire");
  tire.geometry.computeBoundingBox();
  const tireBox=tire.geometry.boundingBox.clone().applyMatrix4(tire.matrixWorld);
  check(Math.abs(tireBox.max.z-tireBox.min.z-68)<1e-4, "Wheel actual outside diameter must be 68 cm");
  check(Math.abs(tireBox.min.z-bike.groundZ)<1e-4, "Neutral tire must meet the ground, not cockpit grid");
  passed.push("Official 49 geometry, real 170 mm cranks/110 mm stem and grounded wheels");
  setRiderDimensions(bike,185,45);
  world.scene.updateMatrixWorld(true);
  for (const side of [-1,1]) {
    check(close(lengthOf(bike.rider.limbs[side].tibia),45), "Both tibias preserve configured knee-to-ankle length");
    check(close(lengthOf(bike.rider.limbs[side].thigh),185*0.485-45), "Height drives estimated remaining leg length");
  }
  const leg=bike.rider.limbs[-1];
  const footHeight=leg.ankle.getWorldPosition(new THREE.Vector3()).z
    -new THREE.Box3().setFromObject(leg.shoe).min.z;
  const standingHeight=footHeight+lengthOf(leg.tibia)+lengthOf(leg.thigh)
    +lengthOf(bike.rider.torso,"z")+lengthOf(bike.rider.neck)+lengthOf(bike.rider.head,"z");
  check(close(standingHeight,185),
    "Rendered segment lengths correspond to configured standing height, not seated bounding box");
  check(close(lengthOf(bike.rider.leftForearm),26), "Left arm IK preserves reference forearm length");
  for (const [height,tibia] of [[NaN,40],[170,-1],[170,60],[300,40]]) {
    let rejected=false;
    try {setRiderDimensions(bike,height,tibia);} catch {rejected=true;}
    check(rejected && close(bike.dimensions.tibia,45), "Invalid dimensions reject without changing last valid model");
  }
  const straight=twoLinkJoint(new THREE.Vector3(),new THREE.Vector3(3,0,0),1,2,new THREE.Vector3(0,0,1));
  check(straight.distanceTo(new THREE.Vector3(1,0,0))<1e-8, "IK supports fully extended boundary");
  check(twoLinkJoint(new THREE.Vector3(),new THREE.Vector3(4,0,0),1,2,new THREE.Vector3(0,0,1))===null,
    "IK must explicitly reject unreachable endpoints");
  setRiderDimensions(bike,170,40);
  passed.push("Configurable height/tibia keep exact bone lengths; invalid proportions and unreachable IK reject");
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
  check(!bike.rider.limbs[-1].upperArm.visible, "Missing IMU must not fabricate a right arm connection");
  world.scene.traverse(object=>{
    object.geometry?.dispose();
    if (object.material) {
      object.material.map?.dispose();
      object.material.dispose();
    }
  });
  return {passed, count:passed.length};
}

export async function runUploadValidationTest() {
  const input=document.getElementById("file");
  const error=document.getElementById("error"), status=document.getElementById("status");
  const ids=["wrist-x","wrist-y","wrist-z","north-heading"];
  const saved=ids.map(id=>document.getElementById(id).value);
  const files=input.files, oldError=error.textContent, oldStatus=status.textContent;
  const originalFetch=globalThis.fetch;
  let requests=0;
  try {
    const transfer=new DataTransfer();
    transfer.items.add(new File(["must not reach the server"],"blocked.csv",{type:"text/csv"}));
    input.files=transfer.files;
    ["-X","+Y","+Z","0"].forEach((value,index)=>{
      document.getElementById(ids[index]).value=value;
    });
    globalThis.fetch=(...args)=>{requests++;return originalFetch(...args);};
    await document.getElementById("upload").onclick();
    if (requests || status.textContent!==oldStatus || !error.textContent.includes("上传未开始")) {
      throw new Error("Invalid view parameters must block upload, keep prior status and show the validation error");
    }
    return {passed:["Invalid scene parameters block upload without API requests or losing prior data"],count:1};
  } finally {
    globalThis.fetch=originalFetch;
    input.files=files;
    ids.forEach((id,index)=>{document.getElementById(id).value=saved[index];});
    error.textContent=oldError;
    status.textContent=oldStatus;
  }
}
