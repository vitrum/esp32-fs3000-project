import "/aero.js";

const chinaTimeFormat=new Intl.DateTimeFormat("zh-CN",{
  timeZone:"Asia/Shanghai",year:"numeric",month:"2-digit",day:"2-digit",
  hour:"2-digit",minute:"2-digit",second:"2-digit",fractionalSecondDigits:3,hourCycle:"h23",
});

export function formatChinaTime(utcMs) {
  return `${chinaTimeFormat.format(new Date(utcMs))} (UTC+08:00)`;
}

export function prepareActivity(records,parameters=globalThis.aeroProbeEstimate.defaults) {
  globalThis.aeroProbeEstimate.validate(parameters);
  const unique=[];
  for (const record of records) {
    for (const key of ["power_w","cadence_rpm","wind_mps"]) {
      if (record[key]!==null && (!Number.isFinite(record[key]) || record[key]<0)) {
        throw new Error(`FIT ${key} 必须是非负有限值或 null。`);
      }
    }
    if (!Number.isFinite(record.utc_ms) || (unique.length && record.utc_ms<unique.at(-1).utc_ms)) {
      throw new Error("FIT 时间轴无效或倒序。");
    }
    if (unique.at(-1)?.utc_ms===record.utc_ms) unique[unique.length-1]=record;
    else unique.push(record);
  }
  let start=0, total=0, count=0;
  return unique.map(record=>{
    if (record.power_w!==null) {total+=record.power_w;count++;}
    while (unique[start].utc_ms<record.utc_ms-2000) {
      if (unique[start].power_w!==null) {total-=unique[start].power_w;count--;}
      start++;
    }
    const power3=count ? total/count : null;
    const estimate=globalThis.aeroProbeEstimate.calculate(
      record.power_w===null ? null : power3,record.wind_mps,parameters);
    return {...record,power3_w:power3,cda:estimate.cda,cda_reason:estimate.reason,
      averaging_samples:count};
  });
}

export function activityAt(records,imuTimeMs,offsetSeconds=0) {
  if (!records.length) return {record:null,reason:"missing_fit",utc_ms:null};
  if (!Number.isFinite(offsetSeconds) || Math.abs(offsetSeconds)>86400) {
    throw new Error("FIT 对齐偏移必须为 -86400 到 86400 秒。");
  }
  const time=records[0].utc_ms+imuTimeMs+offsetSeconds*1000;
  let low=0, high=records.length;
  while (low<high) {
    const middle=(low+high)>>>1;
    if (records[middle].utc_ms<=time) low=middle+1;
    else high=middle;
  }
  const index=low-1;
  const stale=index<0 || time-records[index].utc_ms>2000;
  const ended=time>records.at(-1).utc_ms;
  return {record:stale || ended ? null : records[index],
    reason:ended ? "outside_fit" : stale ? "stale_fit" : null,utc_ms:time};
}
