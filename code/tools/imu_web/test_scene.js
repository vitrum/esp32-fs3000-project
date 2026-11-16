import * as THREE from "three";
import {applyOrientation, armKinematics, createWorld, DIMENSIONS, poseAt, quaternionFromRows,
  RIGHT_GRIP, updateWorld, wristAxisMapping, swapWristAxes, setTextVisibility} from "/scene.js";
import {setRiderDimensions, twoLinkJoint} from "/bike_model.js";
import {prepareActivity, activityAt, formatChinaTime} from "/activity.js";

export function runTests() {
  const passed = [];
  function check(condition, message) {
    if (!condition) throw new Error(message);
  }
  const close = (a,b) => Math.abs(a-b)<1e-8;
  check(formatChinaTime(Date.UTC(2026,10,15,16,0,0,123))==="2026/11/16 00:00:00.123 (UTC+08:00)",
    "Beijing time must roll to the next date at midnight and preserve milliseconds");
  check(formatChinaTime(Date.UTC(2026,11,31,16))==="2027/01/01 00:00:00.000 (UTC+08:00)",
    "Beijing time must handle year rollover independently of the browser timezone");
  passed.push("Absolute FIT times display in China UTC+08 with date/year rollover and milliseconds");
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
  const neutralMapping=["+X","+Y","+Z"];
  const expectedSwaps={XY:["+Y","-X","+Z"],YZ:["+X","+Z","-Y"],XZ:["+Z","+Y","-X"]};
  const reachable=new Set(), pending=[neutralMapping];
  while (pending.length) {
    const mapping=pending.pop(), key=mapping.join();
    if (reachable.has(key)) continue;
    reachable.add(key);
    for (const pair of ["XY","YZ","XZ"]) {
      for (const sign of [1,-1]) {
        const changed=swapWristAxes(mapping,pair,sign);
        wristAxisMapping(changed);
        check(swapWristAxes(changed,pair,-sign).join()===key,"Opposite signed swaps must undo each other");
        check(swapWristAxes(swapWristAxes(changed,pair,sign),pair,sign).join()===swapWristAxes(mapping,pair,-sign).join(),
          "Four same signed swaps restore the original mapping");
        pending.push(changed);
      }
    }
  }
  for (const [pair,expected] of Object.entries(expectedSwaps)) {
    check(swapWristAxes(neutralMapping,pair,1).join()===expected.join(),`${pair}: signed basis must match`);
  }
  check(reachable.size===24 && neutralMapping.join()==="+X,+Y,+Z",
    "Signed swaps reach all 24 proper orientations without mutating their input");
  for (const [mapping,pair,sign] of [
    [neutralMapping,"XX",1],[neutralMapping,"XY",0],[["-X","+Y","+Z"],"XY",1],
  ]) {
    let rejected=false;
    try {swapWristAxes(mapping,pair,sign);} catch {rejected=true;}
    check(rejected,"Invalid or reflected signed swaps must be rejected");
  }
  passed.push("Signed XY/YZ/XZ swaps reach 24 proper orientations with inverse/reset and no mirrors");
  const calibration={
    playback_from_arm:[[-0.5,0,Math.sqrt(.75)],[Math.sqrt(.75),0,0.5],[0,1,0]],
    sensor_reference:[[0,0,1],[0,1,0],[-1,0,0]],
  };
  const calibrated=updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true},
    {mountCalibration:calibration});
  const calibratedCenter=calibrated.kinematics.center.clone();
  const calibratedOrientation=world.wt.getObjectByName("attitude").quaternion.clone();
  const calibratedTurn=updateWorld(world,{board:quaternion,wt:quaternion,paired:true},
    {mountCalibration:calibration});
  world.scene.updateMatrixWorld(true);
  check(calibratedTurn.kinematics.center.distanceTo(calibratedCenter.applyQuaternion(quaternion))<1e-7,
    "Fixed sensor mounting preserves common turn and actual lever length");
  check(world.wt.getObjectByName("attitude").quaternion.angleTo(
    quaternion.clone().multiply(calibratedOrientation))<1e-7,"Calibrated arm turns once, not twice");
  const nativeX=world.wt.getObjectByName("sensor-axes").getObjectByName("X");
  const actualX=new THREE.Vector3(0,1,0).applyQuaternion(nativeX.getWorldQuaternion(new THREE.Quaternion()));
  const expectedNative=quaternion.clone().multiply(quaternionFromRows(calibration.sensor_reference));
  check(actualX.distanceTo(new THREE.Vector3(1,0,0).applyQuaternion(expectedNative))<1e-7,
    "Native arrows remove model-axis permutation instead of masquerading as arm axes");
  check(close(calibratedTurn.kinematics.wrist.distanceTo(calibratedTurn.kinematics.elbow),26),
    "Calibration never scales forearm length");
  passed.push("Fixed mounting preserves common turns, native arrows and forearm length");
  const common=new THREE.Quaternion().setFromEuler(new THREE.Euler(.6,-.7,.8,"ZYX"));
  const bikeTurn=new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0,0,1),.8);
  const armLift=new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0,1,0),.5);
  for (const mountCalibration of [null,calibration]) {
    for (const wristMount of [identityQuaternion,wristAxisMapping(["+Y","-X","+Z"])]) {
      for (const mode of ["hood","free"]) {
        const options={mountCalibration,wristMount,mode};
        for (const movement of [identityQuaternion,armLift]) {
          const neutral=updateWorld(world,{board:identityQuaternion,wt:movement,paired:true,boardHeading:0},options);
          const neutralOrientation=world.wt.getObjectByName("attitude").quaternion.clone();
          const combined=updateWorld(world,{board:common,wt:common.clone().multiply(movement),
            paired:true,boardHeading:THREE.MathUtils.radToDeg(.8)},options);
          const relativeOrientation=bikeTurn.clone().invert()
            .multiply(world.wt.getObjectByName("attitude").quaternion);
          check(relativeOrientation.angleTo(neutralOrientation)<1e-7,
            "Common 3D motion must not change calibrated or manually mapped bike-relative arm orientation");
          for (const point of ["elbow","wrist","hand","center"]) {
            check(combined.kinematics[point].clone().applyQuaternion(bikeTurn.clone().invert())
              .distanceTo(neutral.kinematics[point])<1e-7,
            "Common tilt/turn must preserve every arm lever point in the bike frame");
          }
          check(Math.abs(combined.angle-neutral.angle)<1e-5 && close(combined.armRatio,neutral.armRatio),
            "Relative angles and upper-arm ratios must be invariant to common rigid motion");
          if (mountCalibration) {
            world.scene.updateMatrixWorld(true);
            const nativeX=world.wt.getObjectByName("sensor-axes").getObjectByName("X");
            const actual=new THREE.Vector3(0,1,0).applyQuaternion(
              nativeX.getWorldQuaternion(new THREE.Quaternion()));
            const expected=new THREE.Vector3(1,0,0).applyQuaternion(bikeTurn.clone().multiply(movement)
              .multiply(quaternionFromRows(mountCalibration.sensor_reference)));
            check(actual.distanceTo(expected)<1e-7,
              "Native arrows must preserve board-relative sensor motion after mounting and manual remapping");
          }
        }
      }
    }
  }
  const boardOnly=updateWorld(world,{board:armLift,wt:identityQuaternion,paired:true,boardHeading:0});
  const armOnly=updateWorld(world,{board:identityQuaternion,wt:armLift,paired:true,boardHeading:0});
  check(close(boardOnly.angle,THREE.MathUtils.radToDeg(.5)) && close(armOnly.angle,boardOnly.angle),
    "Independent board and arm tilt must remain visible as relative motion, not be discarded");
  check(boardOnly.kinematics.elbow.distanceTo(armOnly.kinematics.elbow)>20,
    "Board-only tilt and arm-only lift must produce opposite relative arm directions");
  passed.push("Full rigid-frame cancellation preserves independent motion, mounting, support and arm ratios");
  const record=(utc_ms,power_w=300,wind_mps=10)=>({
    utc_ms,power_w,wind_mps,cadence_rpm:0,
  });
  const activity=prepareActivity([record(100000),record(101000,600),record(102000,0),record(103000,600)]);
  check(close(activity[2].power3_w,300) && close(activity[3].power3_w,400),
    "Three-second power uses actual UTC window including valid zero");
  check(close(activity[2].cda,(300*.975-85*9.80665*.004*10)/(.5*1.225*1000)),
    "HUD calls shared simulator CdA with known power and m/s wind");
  check(activity[2].power_w===0 && activity[2].cadence_rpm===0,"Valid zero must remain zero");
  check(prepareActivity([record(100000),record(101000,null)])[1].cda_reason==="missing_power",
    "Missing current power cannot inherit prior power as available");
  check(prepareActivity([record(100000,300,null)])[0].cda===null,"No GPS-speed fallback for absent wind");
  check(prepareActivity([record(100000,300,2.99)])[0].cda_reason==="low_wind",
    "Below 3 m/s rejects CdA");
  check(prepareActivity([record(100000,300,3)])[0].cda!==null,"Exactly 3 m/s accepts positive aerodynamic power");
  const duplicate=prepareActivity([record(100000,300),record(100000,0),record(101000,600)]);
  check(duplicate.length===2 && close(duplicate[1].power3_w,300),"Last duplicate wins without double-weighting");
  passed.push("FIT rolling power, zero/missing values, shared CdA and wind threshold");
  check(activityAt(activity,500).record.power_w===300 && activityAt(activity,500,1).record.power_w===600,
    "Positive manual offset advances FIT relative to IMU");
  check(activityAt(activity,0,-1).record===null && activityAt(activity,3001).reason==="outside_fit",
    "Outside FIT range must not invent telemetry");
  check(activityAt(prepareActivity([record(100000),record(110000)]),5000).record===null,
    "Sparse FIT gaps expire after two seconds");
  passed.push("Approximate start alignment, offsets, end boundaries and stale FIT gaps");
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
  for (const [yaw,pitch,roll] of [[30,40,-35],[-90,-70,55],[170,90,-150]]) {
    const full=new THREE.Quaternion().setFromEuler(new THREE.Euler(
      ...[roll,pitch,yaw].map(THREE.MathUtils.degToRad),"ZYX"));
    const grounded=updateWorld(world,{board:full,wt:full,paired:true,boardHeading:yaw},{initialHeading:23});
    world.scene.updateMatrixWorld(true);
    const expectedBike=new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0,0,1),
      THREE.MathUtils.degToRad(yaw-23));
    check(world.board.getObjectByName("attitude").getWorldQuaternion(new THREE.Quaternion())
      .angleTo(expectedBike)<1e-7,"Computer must be rigidly attached to the grounded frame");
    check(world.board.getObjectByName("attitude").quaternion.angleTo(identityQuaternion)<1e-7 &&
      world.board.position.length()<1e-8,"Computer pose must remain fixed in its cockpit mount");
    check(close(grounded.angle,0),"Shared pitch and roll must not appear as independent arm motion");
    check(close(grounded.heading,(23-yaw+360)%360),"Grounded bike uses integrated heading, even at vertical pitch");
    for (const wheel of [frontWheel,rearWheel]) {
      const tire=wheel.getObjectByName("tire"), positions=tire.geometry.attributes.position;
      let bottom=Infinity;
      for (let index=0;index<positions.count;index++) {
        bottom=Math.min(bottom,new THREE.Vector3().fromBufferAttribute(positions,index)
          .applyMatrix4(tire.matrixWorld).z);
      }
      check(Math.abs(bottom-bike.groundZ)<1e-6,"Both actual tire vertex minima must remain on reference ground");
      check(close(wheel.getWorldPosition(new THREE.Vector3()).z,bike.groundZ+34),
        "Both axle heights remain fixed above the ground");
    }
    check(grounded.kinematics.hand.distanceTo(world.cockpit.localToWorld(new THREE.Vector3(...RIGHT_GRIP)))<1e-7,
      "Arm support must use the same grounded bike frame as the hood");
  }
  updateWorld(world,{board:identityQuaternion,wt:identityQuaternion,paired:true});
  passed.push("Both tires stay grounded through yaw/pitch/roll with rigid computer and hood support");
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
  const headingRows=degrees=>{
    const matrix=new THREE.Matrix4().makeRotationZ(THREE.MathUtils.degToRad(degrees));
    return [0,1,2].map(row=>[0,1,2].map(column=>matrix.elements[column*4+row]));
  };
  const headingFrames=[{...frames[0],board:headingRows(350),board_heading_change_deg:350},
    {...frames[1],board:headingRows(370),board_heading_change_deg:370}];
  check(close(poseAt(headingFrames,50).boardHeading,360),"Unwrapped heading interpolates continuously across north");
  check(poseAt(headingFrames,500).boardHeading===undefined,"Stale board cannot supply a fabricated bike heading");
  passed.push("Grounded heading preserves north-crossing interpolation and invalid-data policy");
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
  const fitOffset=document.getElementById("fit-offset"), oldFitOffset=fitOffset.value;
  const sceneError=document.getElementById("scene-error"), panel=document.getElementById("config-panel");
  const oldSceneError=sceneError.textContent, oldPanelHidden=panel.hidden;
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

    ids.forEach((id,index)=>{document.getElementById(id).value=saved[index];});
    fitOffset.value="100000";
    await document.getElementById("upload").onclick();
    if (requests || status.textContent!==oldStatus || !error.textContent.includes("FIT")) {
      throw new Error("Invalid FIT alignment must also block CSV upload without clearing its error");
    }
    return {passed:["Invalid scene parameters block upload without API requests or losing prior data",
      "Invalid FIT alignment blocks CSV upload and preserves prior analysis"],count:2};
  } finally {
    globalThis.fetch=originalFetch;
    input.files=files;
    fitOffset.value=oldFitOffset;
    ids.forEach((id,index)=>{document.getElementById(id).value=saved[index];});
    error.textContent=oldError;
    status.textContent=oldStatus;
    sceneError.textContent=oldSceneError;
    panel.hidden=oldPanelHidden;
    document.getElementById("panel-toggle").setAttribute("aria-expanded",String(!oldPanelHidden));
  }
}

export function runMountAxisControlTest() {
  const $=id=>document.getElementById(id);
  const ids=["wrist-x","wrist-y","wrist-z"], saved=ids.map(id=>$(id).value);
  const mapping=()=>ids.map(id=>$(id).value).join();
  const defaultMapping="+Y,-X,+Z";
  const expected={XY:["-X,-Y,+Z","+X,+Y,+Z"],YZ:["+Y,+Z,+X","+Y,-Z,-X"],
    XZ:["+Z,-X,-Y","-Z,-X,+Y"]};
  try {
    for (const [pair,orientations] of Object.entries(expected)) {
      for (const [index,sign] of [1,-1].entries()) {
        $("wrist-reset").click();
        const button=document.querySelector(`[data-wrist-swap="${pair}"][data-sign="${sign}"]`);
        if (!button || !button.closest("details").contains($("diagnose"))) {
          throw new Error("Signed swap controls must be inside wearing diagnosis");
        }
        button.click();
        if (mapping()!==orientations[index] || $("error").textContent) {
          throw new Error(`${pair} ${sign}: actual UI handler did not apply the signed swap`);
        }
        document.querySelector(`[data-wrist-swap="${pair}"][data-sign="${-sign}"]`).click();
        if (mapping()!==defaultMapping) throw new Error("Inverse UI swaps must restore the default mapping");
      }
    }
    $("wrist-z").value="-Z";
    document.querySelector('[data-wrist-swap="XY"][data-sign="1"]').click();
    if (!$("error").textContent.includes("右手系") || mapping()!=="+Y,-X,-Z") {
      throw new Error("Reflected input must show an error without being replaced silently");
    }
    $("wrist-reset").click();
    if (mapping()!==defaultMapping || $("error").textContent) throw new Error("Reset must restore +Y/-X/+Z and clear invalid mapping");
    return {passed:["All six signed swap controls apply/invert from wearing diagnosis",
      "Reflected UI input is rejected and reset recovers"],count:2};
  } finally {
    ids.forEach((id,index)=>{$(id).value=saved[index];});
    $("wrist-x").dispatchEvent(new Event("change"));
  }
}
