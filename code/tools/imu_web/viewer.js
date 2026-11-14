import * as THREE from "three";
import {OrbitControls} from "/vendor/OrbitControls.js";
import {createWorld, updateWorld, poseAt} from "/scene.js";

export function start() {
  const $ = id => document.getElementById(id);
  const container = $("viewport");
  const world = createWorld();
  const {scene} = world;
  const renderer = new THREE.WebGLRenderer({antialias:true});
  renderer.setPixelRatio(Math.min(devicePixelRatio, 2));
  container.append(renderer.domElement);
  const camera = new THREE.PerspectiveCamera(42, 1, 0.1, 2500);
  camera.up.set(0, 0, 1);
  const controls = new OrbitControls(camera, renderer.domElement);
  controls.enableDamping = true;
  controls.minDistance = 25;
  controls.maxDistance = 1500;
  controls.target.set(7.5, -5, 0);
  let frames = [], playing = false, anchor = 0, anchorTime = 0, lastIndex = -1;
  let currentTime = 0;
  let settings = {initialHeading:0,mode:"hood",offset:[0,0,0]};
  const error = message => {$("error").textContent = message;};
  function pause() {playing=false; $("play").textContent="播放";}

  function setCamera() {
    const elevation = THREE.MathUtils.degToRad(Number($("elev").value));
    const azimuth = THREE.MathUtils.degToRad(Number($("azim").value));
    const radius = Math.max(95, camera.position.distanceTo(controls.target));
    camera.position.copy(controls.target).add(new THREE.Vector3(
      radius * Math.cos(elevation) * Math.cos(azimuth),
      radius * Math.cos(elevation) * Math.sin(azimuth),
      radius * Math.sin(elevation),
    ));
    controls.update();
  }
  function resetCamera() {
    controls.target.set(-8,0,-3);
    camera.position.copy(controls.target).add(new THREE.Vector3(55,-75,80));
    $("elev").value=35; $("azim").value=-65;
    setCamera();
  }
  function mountChanged() {
    const coordinates = ["x","y","z"].map(name => $(name).valueAsNumber);
    if (!coordinates.every(value => Number.isFinite(value) && Math.abs(value)<=360)) {
      error("安装坐标必须是 -360 到 360 cm 之间的有效数字。");
      return;
    }
    error("");
    const heading=$("north-heading").valueAsNumber;
    if (!Number.isFinite(heading) || heading<0 || heading>360) {
      error("初始参考航向必须是 0 到 360° 之间的数字。");
      return;
    }
    settings={initialHeading:heading,mode:$("arm-mode").value,offset:coordinates};
    if (frames.length) show(currentTime,true);
    else updateWorld(world,{board:new THREE.Quaternion(),wt:new THREE.Quaternion(),paired:true},settings);
  }
  function show(time, force=false) {
    if (!frames.length) return;
    const pose = poseAt(frames, time);
    const motion=updateWorld(world,pose,settings);
    $("timeline").value=pose.index;
    renderer.domElement.dataset.frame=pose.index;
    if (pose.index!==lastIndex || force || playing) {
      $("clock").textContent=`${(time/1000).toFixed(3)} s · 样本 ${pose.index+1}/${frames.length}`;
      const angle = pose.angle===null ? "不可用（未配对或数据间断）" : `${pose.angle.toFixed(1)}°`;
      const heading = motion.heading===null ? "不可用" : `${motion.heading.toFixed(1)}°`;
      const position = motion.kinematics
        ? motion.kinematics.wrist.toArray().map(value=>value.toFixed(1)).join(", ") : "不可用";
      $("pose").textContent=`参考航向 ${heading}（有漂移） · 相对转角 ${angle} · 腕部模型位置 (${position}) cm · ${settings.mode==="hood" ? "手变头支撑" : "离把活动"}${motion.kinematics ? "" : " · 定位不可用"}`;
    }
    lastIndex=pose.index;
  }
  $("upload").onclick=async()=>{
    const file=$("file").files[0];
    if (!file) {error("请先选择 CSV 文件"); return;}
    pause(); error(""); $("upload").disabled=true;
    $("status").textContent="正在上传并分析全部样本…";
    try {
      const response=await fetch("/api/upload", {method:"POST", headers:{"Content-Type":"text/csv"}, body:file});
      const result=await response.json();
      if (!response.ok) throw new Error(result.error);
      $("status").textContent="正在载入全部姿态到浏览器…";
      const playback=await fetch("/api/playback?id="+result.id);
      const data=await playback.json();
      if (!playback.ok) throw new Error(data.error);
      frames=data.frames;
      currentTime=frames[0].time_ms; lastIndex=-1;
      $("timeline").max=frames.length-1;
      for (const id of ["timeline","play","reset"]) $(id).disabled=false;
      $("summary").textContent=JSON.stringify(result.summary,null,2);
      $("download").href="/api/analysis?id="+result.id; $("download").hidden=false;
      $("status").textContent=`分析完成：${frames.length} 行，${result.summary.paired_rows} 个可用同步样本。Three.js 本地渲染，无逐帧网络请求。`;
      show(currentTime,true);
    } catch (failure) {
      error(failure.message);
      $("status").textContent="上传或分析失败；错误详情见上方。";
    } finally {$("upload").disabled=false;}
  };
  $("play").onclick=()=>{
    if (playing) {pause();return;}
    if (currentTime>=frames.at(-1).time_ms) currentTime=frames[0].time_ms;
    anchor=performance.now(); anchorTime=currentTime;
    playing=true; $("play").textContent="暂停";
  };
  $("reset").onclick=()=>{pause();currentTime=frames[0].time_ms;show(currentTime,true);};
  $("timeline").oninput=()=>{
    pause();currentTime=frames[Number($("timeline").value)].time_ms;show(currentTime,true);
  };
  $("speed").onchange=()=>{anchor=performance.now();anchorTime=currentTime;};
  $("elev").oninput=setCamera; $("azim").oninput=setCamera;
  $("camera-reset").onclick=resetCamera;
  for (const name of ["x","y","z"]) $(name).oninput=mountChanged;
  $("north-heading").oninput=mountChanged;
  $("arm-mode").onchange=mountChanged;
  new ResizeObserver(()=>{
    const width=container.clientWidth, height=container.clientHeight;
    camera.aspect=width/height; camera.updateProjectionMatrix();
    renderer.setSize(width,height);
  }).observe(container);
  renderer.domElement.addEventListener("webglcontextlost", event=>{
    event.preventDefault();pause();error("WebGL 上下文丢失，请刷新页面并重新上传 CSV。");
  });
  resetCamera();
  renderer.setAnimationLoop(now=>{
    if (playing) {
      currentTime=Math.min(frames.at(-1).time_ms, anchorTime+(now-anchor)*Number($("speed").value));
      show(currentTime);
      if (currentTime>=frames.at(-1).time_ms) pause();
    }
    controls.update();
    renderer.render(scene,camera);
  });
  $("upload").disabled=false;
  $("status").textContent="Three.js 已就绪，请选择 IMU CSV。";
}
