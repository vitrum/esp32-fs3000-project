import * as THREE from "three";
import {OrbitControls} from "/vendor/OrbitControls.js";
import {createWorld, updateWorld, poseAt, wristAxisMapping, swapWristAxes, mountedWristQuaternion,
  setTextVisibility, RIGHT_GRIP} from "/scene.js";
import {setRiderDimensions, twoLinkJoint} from "/bike_model.js";
import {prepareActivity, activityAt, formatChinaTime} from "/activity.js";

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
  let datasetId=null, diagnostics=null, diagnosticSignature=null;
  let fitRaw=[], fitRecords=[], fitOffset=0, aeroParameters={...globalThis.aeroProbeEstimate.defaults};
  function panel(open) {
    $("config-panel").hidden=!open;
    $("panel-toggle").setAttribute("aria-expanded",String(open));
  }
  const error = message => {
    $("error").textContent=message;
    $("scene-error").textContent=message;
    if (message) panel(true);
  };
  function pause() {playing=false; $("play").textContent="播放";}
  function mountSignature() {
    return JSON.stringify(["x","y","z","arm-mode","rider-height","rider-tibia",
      "wrist-x","wrist-y","wrist-z"].map(id=>$(id).value));
  }
  function invalidateDiagnosis() {
    if (diagnostics && diagnosticSignature!==mountSignature()) {
      $("apply-mount").disabled=true;
      $("mount-status").textContent="身材、支撑或显示轴已改变，请重新诊断；已应用的固定佩戴旋转暂时保留。";
    }
  }
  const reasonText={
    missing_fit:"未载入 FIT",stale_fit:"FIT 尚未开始或数据超过 2 秒未更新",
    outside_fit:"超出 FIT 时间范围",missing_power:"缺少有效功率",
    missing_wind:"缺少视风字段",no_power:"功率均值为零",low_wind:"视风低于 3 m/s",
    nonpositive_aero:"扣除滚阻后气动功率非正",
  };
  function showTelemetry(time) {
    const sample=activityAt(fitRecords,time,fitOffset);
    const record=sample.record;
    const value=(number,digits=0)=>number===null || number===undefined ? "--" : number.toFixed(digits);
    $("hud-power").textContent=value(record?.power_w);
    $("hud-cadence").textContent=value(record?.cadence_rpm,record?.cadence_rpm%1 ? 1 : 0);
    $("hud-wind").textContent=value(record?.wind_mps===null || !record ? null : record.wind_mps*3.6,1);
    $("hud-cda").textContent=value(record?.cda,3);
    const reason=sample.reason || record?.cda_reason;
    const timeLabel=sample.utc_ms===null ? "" : ` · FIT 北京时间 ${formatChinaTime(sample.utc_ms)}`;
    $("telemetry-note").textContent=`起点近似对齐 ${fitOffset>=0 ? "+" : ""}${fitOffset.toFixed(1)} s${timeLabel} · ${
      reason ? reasonText[reason] : `三秒有效功率均值 ${value(record.power3_w)} W`
    } · 使用配置空气密度 ${aeroParameters.rho} kg/m³`;
  }
  function telemetryChanged() {
    const offset=$("fit-offset").valueAsNumber;
    const parameters=Object.fromEntries(["mass","crr","rho","efficiency"].map(name=>[
      name,$(`aero-${name}`).valueAsNumber,
    ]));
    try {
      if (!Number.isFinite(offset) || Math.abs(offset)>86400) throw new Error("FIT 偏移必须为 -86400 到 86400 秒。");
      const prepared=prepareActivity(fitRaw,parameters);
      fitOffset=offset; aeroParameters=parameters; fitRecords=prepared;
      showTelemetry(currentTime);
      return true;
    } catch (failure) {
      error(`未应用 FIT 配置：${failure.message}`);
      return false;
    }
  }

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
    settings={...settings,initialHeading:heading,mode:$("arm-mode").value,offset:coordinates,wristMount};
    invalidateDiagnosis();
    riderSummary();
    if (frames.length) show(currentTime,true);
    else {
      const motion=updateWorld(world,{board:new THREE.Quaternion(),wt:new THREE.Quaternion(),paired:true},settings);
      $("fit-warning").textContent=motion.fitWarning;
    }
    return telemetryChanged();
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
      $("pose").textContent=`码表与车架刚性固定 · 整车贴地（仅航向） · 参考航向 ${heading}（有漂移） · 臂架相对姿态夹角 ${angle} · 腕部模型位置 (${position}) cm · ${settings.mode==="hood" ? "手变头支撑" : "离把活动"}${motion.kinematics ? "" : " · 定位不可用"}${motion.armRatio===null ? "" : ` · 上臂连接比例 ${motion.armRatio.toFixed(2)}`}`;
    }
    showTelemetry(time);
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
      datasetId=result.id; diagnostics=null; diagnosticSignature=null; settings.mountCalibration=null;
      $("candidate").replaceChildren(); $("candidate").disabled=true;
      $("apply-mount").disabled=true; $("diagnose").disabled=false;
      $("mount-report").textContent=""; $("mount-detail").textContent="";
      $("outlier-select").replaceChildren(); $("outlier-select").disabled=true;
      $("mount-status").textContent="新日志未校准；请选择实际握住右手变头的低运动参考。";
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
  $("fit-upload").onclick=async()=>{
    const file=$("fit-file").files[0];
    if (!file) {error("请先选择 FIT 文件");return;}
    if (!mountChanged()) return;
    error(""); $("fit-upload").disabled=true; $("fit-status").textContent="正在严格解析 FIT 与 CRC…";
    try {
      const response=await fetch("/api/fit-upload",{method:"POST",body:file});
      const result=await response.json();
      if (!response.ok) throw new Error(result.error);
      const playback=await fetch("/api/activity?id="+result.id);
      const data=await playback.json();
      if (!playback.ok) throw new Error(data.error);
      const prepared=prepareActivity(data.records,aeroParameters);
      fitRaw=data.records; fitRecords=prepared;
      $("fit-download").href="/api/activity?id="+result.id; $("fit-download").hidden=false;
      $("fit-status").textContent=`已载入 ${data.summary.records} 条记录；北京时间起点 ${formatChinaTime(data.summary.start_utc_ms)}。${data.summary.warnings.join("；")}`;
      showTelemetry(currentTime);
    } catch (failure) {
      error(`FIT 载入失败：${failure.message}`);
      $("fit-status").textContent="FIT 载入失败；浏览器保留先前活动。";
    } finally {$("fit-upload").disabled=false;}
  };
  function mountConfig() {
    const shoulder=world.bike.layout.rightShoulder;
    const grip=new THREE.Vector3(...RIGHT_GRIP).add(new THREE.Vector3(...settings.offset));
    const length=30, upperArm=world.bike.dimensions.upperArm;
    const elbow=twoLinkJoint(shoulder,grip,upperArm,length,new THREE.Vector3(-1,-1,-.5));
    if (!elbow) throw new Error("当前体型与握持点无法满足二段臂参考；请先检查配置。");
    const mount=mountedWristQuaternion(new THREE.Quaternion(),settings.mountCalibration)
      .multiply(settings.wristMount);
    const matrix=new THREE.Matrix4().makeRotationFromQuaternion(mount);
    const rows=[0,1,2].map(row=>[0,1,2].map(column=>matrix.elements[column*4+row]));
    const reference=$("reference-time").valueAsNumber;
    if (!$("auto-reference").checked && (!Number.isFinite(reference) || reference<0)) {
      throw new Error("校准时间必须是录制范围内的有效秒数。");
    }
    return {mode:settings.mode,shoulder:shoulder.toArray(),grip:grip.toArray(),
      neutral_direction:grip.clone().sub(elbow).normalize().toArray(),
      upper_arm:upperArm,forearm_and_palm:length,display_mount:rows,
      reference_time_s:$("auto-reference").checked ? null : reference};
  }
  function candidateSummary() {
    if (!diagnostics) return;
    const candidate=diagnostics.candidates[Number($("candidate").value)];
    const before=diagnostics.baseline, best=diagnostics.recommended_validation;
    const percentage=statistics=>(statistics.outlier_fraction*100).toFixed(1)+"%";
    $("mount-report").textContent=`参考 ${(diagnostics.reference_time_ms/1000).toFixed(2)} s，${diagnostics.quiet_samples} 个低运动样本；采样 ${diagnostics.sampled_count} 个。\n全量独立同步样本 ${before.samples}：未校准基线超限 ${percentage(before)}，首选候选验证 ${percentage(best)}。\n当前候选 ${candidate.mapping.join(" / ")}：采样超限 ${percentage(candidate.sampled_statistics)}，参考长轴偏差 ${candidate.reference_direction_error_deg.toFixed(1)}°。尚需确认佩戴与右手变头接触，旋转不唯一。`;
  }
  $("diagnose").onclick=async()=>{
    if (!mountChanged() || !datasetId) return;
    const requestedId=datasetId, signature=mountSignature();
    $("diagnose").disabled=true;
    try {
      const config=mountConfig();
      $("mount-status").textContent="正在采样全时轴与超限动作，比较固定右手系佩戴候选…";
      const response=await fetch("/api/mount?id="+requestedId,{
        method:"POST",headers:{"Content-Type":"application/json"},body:JSON.stringify(config),
      });
      const result=await response.json();
      if (!response.ok) throw new Error(result.error);
      if (datasetId!==requestedId) throw new Error("IMU 日志已更新，请重新诊断。");
      diagnostics=result; diagnosticSignature=signature;
      $("candidate").replaceChildren(...result.candidates.map((candidate,index)=>{
        const option=document.createElement("option");
        option.value=index; option.textContent=`${index+1}. ${candidate.mapping.join(" / ")} · 采样超限 ${(100*candidate.sampled_statistics.outlier_fraction).toFixed(1)}%`;
        return option;
      }));
      $("candidate").disabled=false; $("apply-mount").disabled=false;
      $("outlier-select").replaceChildren(...result.outlier_examples.map((example,index)=>{
        const option=document.createElement("option");
        option.value=index; option.textContent=`${(example.time_ms/1000).toFixed(2)} s · 原比例 ${example.original_ratio.toFixed(2)} → 首选 ${example.recommended_ratio.toFixed(2)}`;
        return option;
      }));
      $("outlier-select").disabled=!result.outlier_examples.length;
      $("mount-detail").textContent=JSON.stringify(result,null,2);
      $("mount-status").textContent="诊断完成，尚未应用。确认参考段握住右手变头，再点击确认应用。";
      candidateSummary(); invalidateDiagnosis();
    } catch (failure) {
      error(`佩戴诊断失败：${failure.message}`);
      $("mount-status").textContent="未获得新的可应用佩戴诊断。";
    } finally {$("diagnose").disabled=!datasetId;}
  };
  $("candidate").onchange=candidateSummary;
  $("apply-mount").onclick=()=>{
    if (!diagnostics || !mountChanged() || diagnosticSignature!==mountSignature()) return;
    settings.mountCalibration=diagnostics.candidates[Number($("candidate").value)];
    ["X","Y","Z"].forEach(axis=>{$(`wrist-${axis.toLowerCase()}`).value=`+${axis}`;});
    settings.wristMount.identity(); diagnosticSignature=mountSignature();
    $("mount-status").textContent=`已确认固定佩戴候选 ${settings.mountCalibration.mapping.join(" / ")}；参考 ${(diagnostics.reference_time_ms/1000).toFixed(2)} s。原生轴方向仍为候选，不是厂商认证。`;
    show(currentTime,true);
  };
  $("clear-mount").onclick=()=>{
    settings.mountCalibration=null;
    $("mount-status").textContent="已撤销固定佩戴旋转，恢复零化 IMU 播放参考与当前手动显示轴。";
    if (frames.length) show(currentTime,true);
  };
  $("reference-current").onclick=()=>{
    $("auto-reference").checked=false; $("reference-time").value=(currentTime/1000).toFixed(3);
  };
  $("outlier-select").onchange=()=>{
    const example=diagnostics.outlier_examples[Number($("outlier-select").value)];
    pause();currentTime=example.time_ms;show(currentTime,true);
    $("mount-detail").textContent=JSON.stringify(example,null,2);
  };
  $("panel-toggle").onclick=()=>panel($("config-panel").hidden);
  $("panel-close").onclick=()=>{panel(false);$("panel-toggle").focus();};
  $("config-panel").addEventListener("keydown",event=>{
    if (event.key==="Escape") {panel(false);$("panel-toggle").focus();}
  });
  for (const id of ["fit-offset","aero-mass","aero-crr","aero-rho","aero-efficiency"]) {
    $(id).onchange=mountChanged;
  }
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
  setTextVisibility(world,!$("hide-text").checked);
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
  for (const button of document.querySelectorAll("[data-wrist-swap]")) {
    button.onclick=()=>{
      try {
        const mapping=swapWristAxes(["wrist-x","wrist-y","wrist-z"].map(id=>$(id).value),
          button.dataset.wristSwap,Number(button.dataset.sign));
        ["wrist-x","wrist-y","wrist-z"].forEach((id,index)=>{$(id).value=mapping[index];});
        mountChanged();
      } catch (failure) {
        error(`未应用轴互换：${failure.message}`);
      }
    };
  }
  $("wrist-reset").onclick=()=>{
    for (const id of ["wrist-x","wrist-y","wrist-z"]) {
      const select=$(id);
      select.selectedIndex=[...select.options].findIndex(option=>option.defaultSelected);
    }
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
  mountChanged();
  showTelemetry(currentTime);
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
