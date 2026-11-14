import * as THREE from "three";

export const DIMENSIONS = Object.freeze({
  board: Object.freeze([8, 5, 2]),
  wt: Object.freeze([26, 8, 6]),
});
export const COCKPIT = Object.freeze({
  barWidth:38, computerOffset:8, reach:8, drop:12, palmOffset:4,
});
export const RIGHT_GRIP = Object.freeze([4, -COCKPIT.barWidth / 2, 1.5]);

export function quaternionFromRows(rows) {
  const matrix = new THREE.Matrix4().set(
    ...rows[0], 0, ...rows[1], 0, ...rows[2], 0, 0, 0, 0, 1,
  );
  return new THREE.Quaternion().setFromRotationMatrix(matrix).normalize();
}

export function sampleIndex(frames, time) {
  let low = 0, high = frames.length;
  while (low < high) {
    const middle = (low + high) >>> 1;
    if (frames[middle].time_ms <= time) low = middle + 1;
    else high = middle;
  }
  return Math.max(0, low - 1);
}

export function poseAt(frames, time) {
  const index = sampleIndex(frames, time);
  const frame = frames[index], next = frames[index + 1];
  const gap = next ? next.time_ms - frame.time_ms : 0;
  const stale = time - frame.time_ms > 250;
  const pose = {index, paired: frame.paired && !stale};
  for (const sensor of ["board", "wt"]) {
    const matrix = frame[sensor];
    pose[sensor] = matrix && !stale ? quaternionFromRows(matrix) : null;
    if (pose[sensor] && next?.[sensor] && gap > 0 && gap <= 250) {
      pose[sensor].slerp(quaternionFromRows(next[sensor]),
        THREE.MathUtils.clamp((time - frame.time_ms) / gap, 0, 1));
    }
  }
  pose.angle = pose.paired && pose.board && pose.wt
    ? THREE.MathUtils.radToDeg(pose.board.angleTo(pose.wt)) : null;
  return pose;
}

function label(text, color="#243b53") {
  const canvas = document.createElement("canvas");
  const context = canvas.getContext("2d");
  context.font = "bold 30px system-ui";
  canvas.width = Math.ceil(context.measureText(text).width) + 32;
  canvas.height = 64;
  context.font = "bold 30px system-ui";
  context.textAlign = "center";
  context.textBaseline = "middle";
  context.fillStyle = "rgba(255,255,255,0.9)";
  context.fillRect(0, 0, canvas.width, canvas.height);
  context.fillStyle = color;
  context.fillText(text, canvas.width / 2, canvas.height / 2);
  const texture = new THREE.CanvasTexture(canvas);
  texture.colorSpace = THREE.SRGBColorSpace;
  const sprite = new THREE.Sprite(new THREE.SpriteMaterial({map:texture, depthTest:false}));
  sprite.scale.set(3 * canvas.width / canvas.height, 3, 1);
  return sprite;
}

export function createDevice(sensor) {
  const root = new THREE.Group();
  root.name = sensor;
  const attitude = new THREE.Group();
  attitude.name = "attitude";
  root.add(attitude);
  const dimensions = DIMENSIONS[sensor];
  const geometry = new THREE.BoxGeometry(...dimensions);
  const color = sensor === "board" ? 0x118bb0 : 0xdb904f;
  const body = new THREE.Mesh(geometry, new THREE.MeshStandardMaterial({
    color, roughness:0.65, metalness:0.08,
  }));
  body.name = "reference-block";
  attitude.add(body);
  attitude.add(new THREE.LineSegments(new THREE.EdgesGeometry(geometry),
    new THREE.LineBasicMaterial({color:0x263e50})));
  if (sensor === "board") {
    const screen = new THREE.Mesh(new THREE.BoxGeometry(5.8, 3.5, 0.1),
      new THREE.MeshStandardMaterial({color:0x183747, roughness:0.3}));
    screen.position.z = 1.06;
    attitude.add(screen);
  }
  const axes = new THREE.Group();
  axes.name = "sensor-axes";
  for (const [axis, color, axisName] of [
    [0, 0xe33b42, "X"], [1, 0x208846, "Y"], [2, 0x286be2, "Z"],
  ]) {
    const direction = new THREE.Vector3().setComponent(axis, 1);
    const length = sensor === "wt" ? 9 : dimensions[axis] / 2 + 8;
    const arrow = new THREE.ArrowHelper(direction, new THREE.Vector3(), length, color, 2.2, 1.4);
    arrow.name = axisName;
    axes.add(arrow);
    const axisLabel = label(axisName, `#${color.toString(16).padStart(6,"0")}`);
    axisLabel.scale.set(2.5, 2.5, 1);
    axisLabel.position.copy(direction.multiplyScalar(length + 1.8));
    axes.add(axisLabel);
  }
  if (sensor === "wt") {
    const strap = new THREE.Mesh(new THREE.BoxGeometry(3, 8.5, 6.4),
      new THREE.MeshStandardMaterial({color:0x344354}));
    strap.position.set(10, 0, 0);
    attitude.add(strap);
    const imu = new THREE.Mesh(new THREE.BoxGeometry(2.5, 1.8, 0.6),
      new THREE.MeshStandardMaterial({color:0x087d9d}));
    imu.name = "wrist-imu";
    imu.position.set(dimensions[0] / 2, 0, 3.7);
    attitude.add(imu);
    axes.position.copy(imu.position);
    const palm = new THREE.Mesh(new THREE.BoxGeometry(8, 7, 2.5),
      new THREE.MeshStandardMaterial({color:0xe9b58e, roughness:0.9}));
    palm.position.set(dimensions[0] / 2 + COCKPIT.palmOffset, 0, 0);
    attitude.add(palm);
  }
  attitude.add(axes);
  const title = label(sensor === "board" ? "码表 8 × 5 × 2 cm" : "前臂 26 × 8 × 6 cm");
  title.position.set(0, sensor === "board" ? 9 : -9, -6);
  root.add(title);
  const unavailable = new THREE.Mesh(new THREE.BoxGeometry(...dimensions),
    new THREE.MeshBasicMaterial({color:0x8394a7, wireframe:true, transparent:true, opacity:0.4}));
  unavailable.name = "unavailable";
  unavailable.visible = false;
  root.add(unavailable);
  return root;
}

export function applyOrientation(device, quaternion) {
  const attitude = device.getObjectByName("attitude");
  attitude.visible = quaternion !== null;
  device.getObjectByName("unavailable").visible = quaternion === null;
  if (quaternion) attitude.quaternion.copy(quaternion);
}

export function armKinematics(boardQuaternion, armQuaternion, mode="hood", offset=[0,0,0]) {
  const relative = boardQuaternion.clone().invert().multiply(armQuaternion);
  const direction = new THREE.Vector3(1,0,0).applyQuaternion(relative);
  const grip = new THREE.Vector3(...RIGHT_GRIP).add(new THREE.Vector3(...offset));
  const length = DIMENSIONS.wt[0], palm = COCKPIT.palmOffset;
  const elbow = mode === "hood"
    ? grip.clone().addScaledVector(direction, -(length + palm))
    : grip.clone().add(new THREE.Vector3(-(length + palm),0,0));
  const center = elbow.clone().addScaledVector(direction, length / 2);
  const wrist = elbow.clone().addScaledVector(direction, length);
  const hand = wrist.clone().addScaledVector(direction, palm);
  return Object.fromEntries(Object.entries({elbow,center,wrist,hand})
    .map(([name,point])=>[name,point.applyQuaternion(boardQuaternion)]));
}

function tube(points, radius, color) {
  const curve = new THREE.CatmullRomCurve3(points.map(point=>new THREE.Vector3(...point)));
  return new THREE.Mesh(new THREE.TubeGeometry(curve,64,radius,10,false),
    new THREE.MeshStandardMaterial({color,roughness:0.7}));
}

function directionArrow(name, text, color, position, length) {
  const group = new THREE.Group();
  group.name = name;
  group.position.set(...position);
  const material = new THREE.MeshStandardMaterial({color});
  const shaft = new THREE.Mesh(new THREE.CylinderGeometry(0.55,0.55,length-5,16),material);
  shaft.rotation.z = -Math.PI / 2;
  shaft.position.x = (length-5) / 2;
  group.add(shaft);
  const head = new THREE.Mesh(new THREE.ConeGeometry(2,5,20),material);
  head.rotation.z = -Math.PI / 2;
  head.position.x = length-2.5;
  group.add(head);
  const title = label(text);
  title.position.set(length / 2,0,5);
  group.add(title);
  return group;
}

function cockpitGeometry() {
  const group = new THREE.Group();
  group.name = "cockpit";
  const x = -COCKPIT.computerOffset;
  const barCenter = new THREE.Object3D();
  barCenter.name = "bar-center";
  barCenter.position.set(x,0,-5);
  group.add(barCenter);
  for (const side of [-1,1]) {
    const y = side * COCKPIT.barWidth / 2;
    group.add(tube([
      [x,0,-5],[x,side*12,-5],[x+2,y,-5],[x+8,y,-5],
      [x+12,y,-10],[x+9,y,-17],[x+1,y,-17],
    ],1.1,0x293443));
    const hood = new THREE.Group();
    hood.name = side === -1 ? "right-hood" : "left-hood";
    hood.position.set(x+9,y,-2.5);
    hood.add(new THREE.Mesh(new THREE.BoxGeometry(9,4.2,5),
      new THREE.MeshStandardMaterial({color:0x414d5c,roughness:0.9})));
    const horn = new THREE.Mesh(new THREE.BoxGeometry(3,4,5),
      new THREE.MeshStandardMaterial({color:0x344152}));
    horn.position.set(4,0,2);
    hood.add(horn);
    hood.add(tube([[5,0,0],[7,0,-4],[6,0,-10]],0.35,0x9eaab6));
    const hoodLabel = label(side === -1 ? "右电变手变头 · 支撑参考" : "左手变头");
    hoodLabel.position.set(3,side*7,-7);
    hood.add(hoodLabel);
    group.add(hood);
  }
  const mount = new THREE.Group();
  mount.name = "computer-mount";
  mount.add(tube([[x,0,-5],[x+3,0,-2],[0,0,-1.5]],0.65,0x63768a));
  const ring = new THREE.Mesh(new THREE.TorusGeometry(2.3,0.45,8,32),
    new THREE.MeshStandardMaterial({color:0x3e5164}));
  ring.position.z = -1.5;
  mount.add(ring);
  group.add(mount);
  const width = new THREE.BufferGeometry().setFromPoints([
    new THREE.Vector3(x-5,-19,-22),new THREE.Vector3(x-5,19,-22),
  ]);
  group.add(new THREE.Line(width,new THREE.LineBasicMaterial({color:0x65768a})));
  const widthLabel = label("弯把中对中 38 cm");
  widthLabel.position.set(x-5,0,-22);
  group.add(widthLabel);
  const offsetLabel = label("码表前伸 8 cm");
  offsetLabel.position.set(-4,7,5);
  group.add(offsetLabel);
  const grip = new THREE.Mesh(new THREE.SphereGeometry(1,12,8),
    new THREE.MeshBasicMaterial({color:0xf1ad00,wireframe:true}));
  grip.name = "right-grip";
  grip.position.set(...RIGHT_GRIP);
  group.add(grip);
  return group;
}

export function updateWorld(world, pose, {initialHeading=0, mode="hood", offset=[0,0,0]}={}) {
  const northOffset = new THREE.Quaternion().setFromAxisAngle(new THREE.Vector3(0,0,1),
    -THREE.MathUtils.degToRad(initialHeading));
  const boardQuaternion = pose.board ? northOffset.clone().multiply(pose.board) : null;
  world.cockpit.visible = boardQuaternion !== null;
  if (boardQuaternion) world.cockpit.quaternion.copy(boardQuaternion);
  world.cockpit.getObjectByName("right-grip").position.set(...RIGHT_GRIP)
    .add(new THREE.Vector3(...offset));
  const paired = pose.paired && boardQuaternion && pose.wt;
  world.wt.visible = Boolean(paired);
  world.elbow.visible = Boolean(paired);
  let kinematics = null;
  if (paired) {
    const armQuaternion = northOffset.clone().multiply(pose.wt);
    kinematics = armKinematics(boardQuaternion,armQuaternion,mode,offset);
    world.wt.position.copy(kinematics.center);
    applyOrientation(world.wt,armQuaternion);
    world.elbow.position.copy(kinematics.elbow);
  }
  const forward = boardQuaternion
    ? new THREE.Vector3(1,0,0).applyQuaternion(boardQuaternion) : null;
  const heading = forward && Math.hypot(forward.x,forward.y)>1e-6
    ? (THREE.MathUtils.radToDeg(Math.atan2(-forward.y,forward.x))+360)%360 : null;
  return {kinematics,heading};
}

export function createWorld() {
  const scene = new THREE.Scene();
  scene.background = new THREE.Color(0xedf3fa);
  scene.add(new THREE.HemisphereLight(0xffffff, 0x6f859d, 2.5));
  const light = new THREE.DirectionalLight(0xffffff, 3);
  light.position.set(-20, -35, 75);
  scene.add(light);
  const grid = new THREE.GridHelper(180, 18, 0x8fa9c3, 0xcbd8e6);
  grid.rotation.x = Math.PI / 2;
  grid.position.z = -25;
  scene.add(grid);
  const board = createDevice("board"), wt = createDevice("wt");
  const cockpit = cockpitGeometry();
  const forward = directionArrow("bike-forward","车头方向 · 随码表转向",0x087d9d,[-5,7,-10],20);
  cockpit.add(board,forward);
  const north = directionArrow("magnetic-north","参考磁北 N · 未校准",0xe6a300,[-30,45,-22],28);
  const elbow = new THREE.Mesh(new THREE.SphereGeometry(1.3,12,8),
    new THREE.MeshStandardMaterial({color:0xbe7041}));
  elbow.name = "estimated-elbow";
  scene.add(cockpit,wt,north,elbow);
  const world = {scene,board,wt,cockpit,forward,north,elbow};
  updateWorld(world,{board:new THREE.Quaternion(),wt:new THREE.Quaternion(),paired:true});
  return world;
}
