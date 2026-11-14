import * as THREE from "three";

// Specialized 2021 Comp Disc, size 49; cm. Crank/stem are the user's overrides.
export const BIKE_GEOMETRY = Object.freeze({
  stack:50.7, reach:37.5, headTube:11, headAngle:72.25,
  seatTube:46, seatAngle:74, chainstay:41, bbDrop:6.9,
  wheelbase:96.6, wheelRadius:34, crank:17, stem:11, stemAngle:7,
});

const Z = new THREE.Vector3(0,0,1);
const Y = new THREE.Vector3(0,1,0);
const vector = (x=0,y=0,z=0)=>new THREE.Vector3(x,y,z);
const skin = 0xdca47d;

function marker(parent,name,position) {
  const object=new THREE.Object3D();
  object.name=name;
  object.position.copy(position);
  parent.add(object);
  return object;
}

function placeSegment(mesh,start,end) {
  const delta=end.clone().sub(start);
  mesh.position.copy(start).add(end).multiplyScalar(0.5);
  mesh.scale.y=delta.length();
  mesh.quaternion.setFromUnitVectors(Y,delta.normalize());
}

function segment(parent,name,start,end,radius,color) {
  const mesh=new THREE.Mesh(new THREE.CylinderGeometry(radius,radius,1,12),
    new THREE.MeshStandardMaterial({color,roughness:0.65}));
  mesh.name=name;
  placeSegment(mesh,start,end);
  parent.add(mesh);
  return mesh;
}

function ellipsoid(parent,name,color) {
  const mesh=new THREE.Mesh(new THREE.SphereGeometry(1,24,16),
    new THREE.MeshStandardMaterial({color,roughness:0.85}));
  mesh.name=name;
  parent.add(mesh);
  return mesh;
}

function wheel(parent,name,center) {
  const group=new THREE.Group();
  group.name=name;
  group.position.copy(center);
  const tire=new THREE.Mesh(new THREE.TorusGeometry(32.7,1.3,12,96),
    new THREE.MeshStandardMaterial({color:0x242b32,roughness:0.95}));
  tire.name="tire";
  tire.rotation.x=Math.PI/2;
  const rim=new THREE.Mesh(new THREE.TorusGeometry(31.1,0.6,8,96),
    new THREE.MeshStandardMaterial({color:0x76828b,metalness:0.6,roughness:0.4}));
  rim.rotation.x=Math.PI/2;
  group.add(tire,rim);
  segment(group,"hub",vector(0,-3,0),vector(0,3,0),1.4,0x5e6a76);
  const points=[];
  for (let index=0;index<24;index++) {
    const angle=index*Math.PI/12;
    points.push(vector(0,index%2 ? -1.8 : 1.8,0),
      vector(30.8*Math.cos(angle),0,30.8*Math.sin(angle)));
  }
  group.add(new THREE.LineSegments(new THREE.BufferGeometry().setFromPoints(points),
    new THREE.LineBasicMaterial({color:0x728292})));
  const rotor=new THREE.Mesh(new THREE.RingGeometry(2.4,8,40),
    new THREE.MeshStandardMaterial({color:0xa7b4c2,side:THREE.DoubleSide,metalness:0.7}));
  rotor.rotation.x=Math.PI/2;
  rotor.position.y=3;
  group.add(rotor);
  const caliper=new THREE.Mesh(new THREE.BoxGeometry(3,3,4),
    new THREE.MeshStandardMaterial({color:0x36495b}));
  caliper.position.set(-5,3,6);
  group.add(caliper);
  parent.add(group);
  return group;
}

export function riderDimensions(height=170,tibia=40) {
  if (!Number.isFinite(height) || height<130 || height>220
      || !Number.isFinite(tibia) || tibia<25 || tibia>60) {
    throw new Error("假人身高需为 130–220 cm，胫骨（膝到踝）需为 25–60 cm。");
  }
  const torso=height*0.30, neck=height*0.045, head=height*0.13, footHeight=height*0.04;
  const thigh=height-torso-neck-head-footHeight-tibia;
  if (thigh<height*0.17 || thigh>height*0.32) {
    throw new Error("该身高/胫骨组合使估计大腿比例不合理，请检查尺寸和 cm 单位。");
  }
  return {height,tibia,thigh,torso,neck,head,footHeight,upperArm:height*0.186,
    forearm:26,footLength:height*0.15};
}

export function twoLinkJoint(start,end,firstLength,secondLength,bend) {
  const direction=end.clone().sub(start);
  const distance=direction.length();
  if (distance<1e-8 || distance>firstLength+secondLength
      || distance<Math.abs(firstLength-secondLength)) return null;
  direction.divideScalar(distance);
  const along=(firstLength**2-secondLength**2+distance**2)/(2*distance);
  const lateral=bend.clone().addScaledVector(direction,-bend.dot(direction));
  if (lateral.lengthSq()<1e-10) {
    lateral.copy(Math.abs(direction.z)<0.9 ? Z : Y);
    lateral.addScaledVector(direction,-lateral.dot(direction));
  }
  return start.clone().addScaledVector(direction,along)
    .addScaledVector(lateral.normalize(),Math.sqrt(Math.max(0,firstLength**2-along**2)));
}

function riderLayout(model,dimensions) {
  const d=dimensions;
  const saddle=model.bb.clone().addScaledVector(model.seatDirection,(d.thigh+d.tibia)*0.84)
    .add(vector(-2,0,1.25));
  const pelvis=saddle.clone().add(vector(0,0,d.footHeight));
  const lean=THREE.MathUtils.degToRad(70);
  const shoulders=pelvis.clone().add(vector(d.torso*Math.sin(lean),0,d.torso*Math.cos(lean)));
  const legs=[];
  for (const side of [-1,1]) {
    const hip=pelvis.clone().add(vector(0,side*d.height*0.065,0));
    const pedal=model.pedals[side];
    const ankle=pedal.clone().add(vector(-d.height*0.06,0,d.footHeight));
    const knee=twoLinkJoint(hip,ankle,d.thigh,d.tibia,vector(1,0,0));
    if (!knee) throw new Error("该身材无法在当前静态曲柄位置保持腿长；请检查身高和胫骨长度。");
    legs.push({side,hip,knee,ankle,pedal});
  }
  const leftShoulder=shoulders.clone().add(vector(0,d.height*0.105,0));
  const leftWrist=vector(0,19,1.5);
  const leftElbow=twoLinkJoint(leftShoulder,leftWrist,d.upperArm,d.forearm,vector(-1,0,-1));
  if (!leftElbow) throw new Error("该身材无法到达左手变头；此 49 码模型的把位仅是参考。");
  const rightShoulder=shoulders.clone().add(vector(0,-d.height*0.105,0));
  return {saddle,pelvis,shoulders,legs,leftShoulder,leftWrist,leftElbow,rightShoulder};
}

function createRider(model,makeLabel) {
  const root=new THREE.Group();
  root.name="mannequin";
  model.root.add(root);
  const torso=ellipsoid(root,"torso",0x426b8e);
  const pelvis=ellipsoid(root,"pelvis",0x263e56);
  const head=ellipsoid(root,"head",skin);
  const neck=segment(root,"neck",vector(),vector(0,0,1),2.7,skin);
  const limbs={};
  for (const side of [-1,1]) {
    const name=side===-1 ? "right" : "left";
    limbs[side]={
      thigh:segment(root,`${name}-thigh`,vector(),vector(0,0,1),4.2,0x263e56),
      tibia:segment(root,`${name}-tibia`,vector(),vector(0,0,1),2.8,skin),
      knee:ellipsoid(root,`${name}-knee`,skin),
      ankle:ellipsoid(root,`${name}-ankle`,skin),
      shoulder:ellipsoid(root,`${name}-shoulder`,0x426b8e),
      upperArm:segment(root,`${name}-upper-arm-reference`,vector(),vector(0,0,1),3.2,skin),
    };
    const shoe=new THREE.Mesh(new THREE.BoxGeometry(1,1,1),
      new THREE.MeshStandardMaterial({color:0xe6eaf0,roughness:0.8}));
    shoe.name=`${name}-shoe`;
    root.add(shoe);
    limbs[side].shoe=shoe;
  }
  const leftForearm=segment(root,"left-forearm",vector(),vector(0,0,1),3,skin);
  const leftHand=new THREE.Mesh(new THREE.BoxGeometry(8,6,2.5),
    new THREE.MeshStandardMaterial({color:skin}));
  leftHand.position.set(4,19,1.5);
  root.add(leftHand);
  const title=makeLabel("假人 · 未测量关节的比例参考");
  root.add(title);
  return {root,torso,pelvis,head,neck,limbs,leftForearm,title};
}

export function setRiderDimensions(model,height=170,tibia=40) {
  const dimensions=riderDimensions(height,tibia);
  const layout=riderLayout(model,dimensions);
  const d=dimensions, r=model.rider;
  model.dimensions=dimensions;
  model.layout=layout;
  model.saddle.position.copy(layout.saddle);
  placeSegment(model.seatpost,model.seatTop,layout.saddle.clone().add(vector(2,0,-1.25)));
  r.pelvis.position.copy(layout.pelvis);
  r.pelvis.scale.set(8,d.height*0.08,d.footHeight);
  r.torso.position.copy(layout.pelvis).add(layout.shoulders).multiplyScalar(0.5);
  r.torso.scale.set(d.height*0.045,d.height*0.105,d.torso/2);
  r.torso.quaternion.setFromUnitVectors(Z,layout.shoulders.clone().sub(layout.pelvis).normalize());
  const headBottom=layout.shoulders.clone().add(vector(0,0,d.neck));
  placeSegment(r.neck,layout.shoulders,headBottom);
  r.head.position.copy(headBottom).add(vector(1,0,d.head/2));
  r.head.scale.set(d.head*0.42,d.head*0.34,d.head/2);
  r.title.position.copy(r.head.position).add(vector(0,0,d.head/2+5));
  for (const leg of layout.legs) {
    const limb=r.limbs[leg.side];
    placeSegment(limb.thigh,leg.hip,leg.knee);
    placeSegment(limb.tibia,leg.knee,leg.ankle);
    limb.knee.position.copy(leg.knee); limb.knee.scale.setScalar(3.4);
    limb.ankle.position.copy(leg.ankle); limb.ankle.scale.setScalar(2.4);
    limb.shoe.scale.set(d.footLength,8,5);
    limb.shoe.position.copy(leg.pedal).add(vector(-d.height*0.03,0,2.5));
    limb.shoulder.position.copy(leg.side===-1 ? layout.rightShoulder : layout.leftShoulder);
    limb.shoulder.scale.setScalar(4);
  }
  placeSegment(r.limbs[1].upperArm,layout.leftShoulder,layout.leftElbow);
  placeSegment(r.leftForearm,layout.leftElbow,layout.leftWrist);
}

export function updateRiderArm(model,elbow) {
  const upperArm=model.rider.limbs[-1].upperArm;
  upperArm.visible=elbow!==null;
  if (!elbow) return "";
  const shoulder=model.layout.rightShoulder;
  placeSegment(upperArm,shoulder,elbow);
  const ratio=elbow.distanceTo(shoulder)/model.dimensions.upperArm;
  const mismatch=ratio<0.7 || ratio>1.15;
  upperArm.material.color.setHex(mismatch ? 0xcd6464 : skin);
  return mismatch ? "右上臂连接超出身材参考比例：检查腕部轴映射和握持点；此连接不是实测关节。" : "";
}

export function createBikeModel(barCenter,makeLabel) {
  const g=BIKE_GEOMETRY;
  const root=new THREE.Group();
  root.name="bike-reference";
  const stemAngle=THREE.MathUtils.degToRad(g.stemAngle);
  const headAngle=THREE.MathUtils.degToRad(g.headAngle);
  const stemStart=barCenter.clone().sub(vector(g.stem*Math.cos(stemAngle),0,g.stem*Math.sin(stemAngle)));
  const headTop=stemStart.clone().add(vector(2*Math.cos(headAngle),0,-2*Math.sin(headAngle)));
  const bb=headTop.clone().sub(vector(g.reach,0,g.stack));
  const headBottom=headTop.clone().add(vector(g.headTube*Math.cos(headAngle),0,-g.headTube*Math.sin(headAngle)));
  const seatAngle=THREE.MathUtils.degToRad(g.seatAngle);
  const seatDirection=vector(-Math.cos(seatAngle),0,Math.sin(seatAngle));
  const seatTop=bb.clone().addScaledVector(seatDirection,g.seatTube);
  const rearDistance=Math.sqrt(g.chainstay**2-g.bbDrop**2);
  const rearAxle=bb.clone().add(vector(-rearDistance,0,g.bbDrop));
  const frontAxle=rearAxle.clone().add(vector(g.wheelbase,0,0));
  const groundZ=bb.z+g.bbDrop-g.wheelRadius;
  for (const [name,point] of Object.entries({bb,headTop,headBottom,seatTop,stemStart,frontAxle,rearAxle})) {
    marker(root,name,point);
  }
  segment(root,"stem",stemStart,barCenter,1.5,0x344555);
  segment(root,"headset-spacer",headTop,stemStart,1.7,0x344555);
  segment(root,"head-tube",headTop,headBottom,2,0x202d3c);
  segment(root,"top-tube",seatTop,headTop,1.7,0x202d3c);
  segment(root,"down-tube",bb,headBottom,2.3,0x202d3c);
  segment(root,"seat-tube",bb,seatTop,1.7,0x202d3c);
  for (const side of [-1,1]) {
    segment(root,`chainstay-${side}`,bb.clone().add(vector(0,side*2.5,0)),
      rearAxle.clone().add(vector(0,side*2.5,0)),0.85,0x263e56);
    segment(root,`seatstay-${side}`,seatTop.clone().add(vector(0,side*2.5,0)),
      rearAxle.clone().add(vector(0,side*2.5,0)),0.7,0x263e56);
    segment(root,`fork-${side}`,headBottom.clone().add(vector(0,side*1.5,0)),
      frontAxle.clone().add(vector(0,side*4.5,0)),1.05,0x253749);
  }
  wheel(root,"front-wheel",frontAxle);
  wheel(root,"rear-wheel",rearAxle);
  segment(root,"crank-spindle",bb.clone().add(vector(0,-9,0)),bb.clone().add(vector(0,9,0)),1.6,0x8795a3);
  const pedals={};
  const crankAngle=Math.PI/6;
  for (const side of [-1,1]) {
    const end=bb.clone().add(vector(-side*g.crank*Math.cos(crankAngle),side*7.5,-side*g.crank*Math.sin(crankAngle)));
    segment(root,`crank-${side}`,bb.clone().add(vector(0,side*7.5,0)),end,1,0x485a6d);
    pedals[side]=end.clone().add(vector(0,side*3.5,0));
    marker(root,`pedal-contact-${side}`,pedals[side]);
    const pedal=new THREE.Mesh(new THREE.BoxGeometry(7,8,2),
      new THREE.MeshStandardMaterial({color:0x344555}));
    pedal.position.copy(pedals[side]);
    root.add(pedal);
  }
  for (const radius of [10,6.8]) {
    const ring=new THREE.Mesh(new THREE.TorusGeometry(radius,0.35,8,64),
      new THREE.MeshStandardMaterial({color:0x8a98a8,metalness:0.6}));
    ring.rotation.x=Math.PI/2;
    ring.position.copy(bb).add(vector(0,-6.5,0));
    root.add(ring);
  }
  for (let index=0;index<11;index++) {
    const cog=new THREE.Mesh(new THREE.TorusGeometry(2.2+index*0.28,0.13,6,32),
      new THREE.MeshStandardMaterial({color:0x8a98a8,metalness:0.6}));
    cog.rotation.x=Math.PI/2;
    cog.position.copy(rearAxle).add(vector(0,-3.5-index*0.3,0));
    root.add(cog);
  }
  const chainPoints=[
    bb.clone().add(vector(0,-6.5,10)),rearAxle.clone().add(vector(0,-6.5,5)),
    rearAxle.clone().add(vector(0,-6.5,-5)),bb.clone().add(vector(0,-6.5,-10)),
  ];
  root.add(new THREE.LineLoop(new THREE.BufferGeometry().setFromPoints(chainPoints),
    new THREE.LineBasicMaterial({color:0x6b7e92})));
  segment(root,"rear-derailleur",rearAxle.clone().add(vector(2,-7,-3)),
    rearAxle.clone().add(vector(4,-7,-10)),1.1,0x354759);
  const seatpost=segment(root,"seatpost",seatTop,seatTop.clone().add(vector(0,0,20)),1.3,0x42576c);
  const saddle=new THREE.Mesh(new THREE.BoxGeometry(26,15.5,2.5),
    new THREE.MeshStandardMaterial({color:0x263849}));
  saddle.name="saddle";
  root.add(saddle);
  const title=makeLabel("2021 Allez Sprint Comp · 49 码参考");
  title.position.copy(bb).add(vector(4,0,-18));
  root.add(title);
  const model={root,bb,headTop,headBottom,seatTop,seatDirection,frontAxle,rearAxle,
    groundZ,pedals,seatpost,saddle};
  model.rider=createRider(model,makeLabel);
  setRiderDimensions(model);
  return model;
}
