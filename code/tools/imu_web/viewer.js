import * as THREE from "three";
import {OrbitControls} from "/vendor/OrbitControls.js";
import {createWorld, updateWorld, poseAt, wristAxisMapping, setTextVisibility} from "/scene.js";
import {setRiderDimensions} from "/bike_model.js";

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
    const radius = Math.max(controls.minDistance, camera.position.distanceTo(controls.target));
    camera.position.copy(controls.target).add(new THREE.Vector3(
      radius * Math.cos(elevation) * Math.cos(azimuth),
      radius * Math.cos(elevation) * Math.sin(azimuth),
      radius * Math.sin(elevation),
    ));
    controls.update();
  }
  function resetCamera() {
    scene.updateMatrixWorld(true);
    controls.target.copy(world.cockpit.localToWorld(world.bike.bb.clone().add(new THREE.Vector3(5,0,48))));
    const radius=Math.max(250,world.bike.dimensions.height*1.3)/Math.min(1,camera.aspect);
    camera.position.copy(controls.target).add(new THREE.Vector3(radius,0,0));
    $("elev").value=22; $("azim").value=-80;
    setCamera();
  }
  function cockpitView() {
    scene.updateMatrixWorld(true);
    controls.target.copy(world.cockpit.localToWorld(new THREE.Vector3(-8,0,-3)));
    camera.position.copy(controls.target).add(new THREE.Vector3(115,0,0));
    $("elev").value=35; $("azim").value=-65;
    setCamera();
  }
  function riderSummary() {
    const d=world.bike.dimensions;
    $("rider-summary").textContent=`已应用：身高 ${d.height} cm，胫骨 ${d.tibia} cm；估计大腿 ${d.thigh.toFixed(1)} cm、躯干 ${d.torso.toFixed(1)} cm。坐垫高度随腿长估计，不是 bike fit。`;
  }
  function mountChanged() {
    const coordinates = ["x","y","z"].map(name => $(name).valueAsNumber);
    if (!coordinates.every(value => Number.isFinite(value) && Math.abs(value)<=360)) {
      error("安装坐标必须是 -360 到 360 cm 之间的有效数字。");
      return false;
    }
    error("");
    const heading=$("north-heading").valueAsNumber;
    if (!Number.isFinite(heading) || heading<0 || heading>360) {
      error("初始参考航向必须是 0 到 360° 之间的数字。");
      return false;
    }
    let wristMount;
    try {
      wristMount=wristAxisMapping(["wrist-x","wrist-y","wrist-z"].map(id=>$(id).value));
      const height=$("rider-height").valueAsNumber, tibia=$("rider-tibia").valueAsNumber;
      if (height!==world.bike.dimensions.height || tibia!==world.bike.dimensions.tibia) {
        setRiderDimensions(world.bike,height,tibia);
      }
    } catch (failure) {
      error(`未应用修改：${failure.message}`);
      return false;
    }
    settings={initialHeading:heading,mode:$("arm-mode").value,offset:coordinates,wristMount};
    riderSummary();
    if (frames.length) show(currentTime,true);
    else {
      const motion=updateWorld(world,{board:new THREE.Quaternion(),wt:new THREE.Quaternion(),paired:true},settings);
      $("fit-warning").textContent=motion.fitWarning;
    }
    return true;
  }
  function show(time, force=false) {
    if (!frames.length) return;
    const pose = poseAt(frames, time);
    const motion=updateWorld(world,pose,settings);
    $("fit-warning").textContent=motion.fitWarning;
    $("timeline").value=pose.index;
    renderer.domElement.dataset.frame=pose.index;
    if (pose.index!==lastIndex || force || playing) {
      $("clock").textContent=`${(time/1000).toFixed(3)} s · 样本 ${pose.index+1}/${frames.length}`;
      const angle = motion.angle===null ? "不可用（未配对或数据间断）" : `${motion.angle.toFixed(1)}°`;
      const heading = motion.heading===null ? "不可用" : `${motion.heading.toFixed(1)}°`;
      const position = motion.kinematics
        ? motion.kinematics.wrist.toArray().map(value=>value.toFixed(1)).join(", ") : "不可用";
      $("pose").textContent=`参考航向 ${heading}（有漂移） · 映射后相对转角 ${angle} · 腕部模型位置 (${position}) cm · ${settings.mode==="hood" ? "手变头支撑" : "离把活动"}${motion.kinematics ? "" : " · 定位不可用"}`;
    }
    lastIndex=pose.index;
  }
  $("upload").onclick=async()=>{
    const file=$("file").files[0];
    if (!file) {error("请先选择 CSV 文件"); return;}
    if (!mountChanged()) {
      error(`上传未开始：请先修正场景参数。${$("error").textContent}`);
      return;
    }
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
  $("cockpit-view").onclick=cockpitView;
  function zoom(factor) {
    const delta=camera.position.clone().sub(controls.target);
    const distance=THREE.MathUtils.clamp(delta.length()*factor,controls.minDistance,controls.maxDistance);
    camera.position.copy(controls.target).add(delta.setLength(distance));
    controls.update();
    renderer.domElement.dataset.cameraDistance=distance;
  }
  $("zoom-in").onclick=()=>zoom(0.8);
  $("zoom-out").onclick=()=>zoom(1.25);
  $("hide-text").onchange=()=>setTextVisibility(world,!$("hide-text").checked);
  $("fullscreen").onclick=async()=>{
    try {
      if (document.fullscreenElement) await document.exitFullscreen();
      else if ($("stage").requestFullscreen) await $("stage").requestFullscreen();
      else throw new Error("当前浏览器不支持 Fullscreen API，请使用浏览器全屏功能。");
      $("view-warning").textContent="";
    } catch (failure) {
      $("view-warning").textContent=`全屏切换失败：${failure.message}`;
    }
  };
  document.addEventListener("fullscreenchange",()=>{
    $("fullscreen").textContent=document.fullscreenElement ? "退出全屏" : "全屏";
    controls.update();
  });
  for (const name of ["x","y","z"]) $(name).oninput=mountChanged;
  $("north-heading").oninput=mountChanged;
  $("arm-mode").onchange=mountChanged;
  $("rider-height").oninput=mountChanged;
  $("rider-tibia").oninput=mountChanged;
  for (const id of ["wrist-x","wrist-y","wrist-z"]) $(id).onchange=mountChanged;
  $("wrist-reset").onclick=()=>{
    ["X","Y","Z"].forEach(axis=>{ $(`wrist-${axis.toLowerCase()}`).value=`+${axis}`; });
    mountChanged();
  };
  let sized=false;
  new ResizeObserver(()=>{
    const width=container.clientWidth, height=container.clientHeight;
    if (!width || !height) return;
    camera.aspect=width/height; camera.updateProjectionMatrix();
    renderer.setSize(width,height);
    if (!sized) {sized=true;resetCamera();}
  }).observe(container);
  renderer.domElement.addEventListener("webglcontextlost", event=>{
    event.preventDefault();pause();error("WebGL 上下文丢失，请刷新页面并重新上传 CSV。");
  });
  resetCamera();
  riderSummary();
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
