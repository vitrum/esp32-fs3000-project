/* Shared by the firmware simulator and the dual-IMU playback HUD. */
(() => {
  const defaults=Object.freeze({mass:85,crr:0.004,rho:1.225,efficiency:0.975});
  function validate(parameters) {
    for (const [name,minimum,maximum] of [
      ["mass",30,250],["crr",0,0.05],["rho",0.5,1.6],["efficiency",0.5,1],
    ]) {
      if (!Number.isFinite(parameters[name]) || parameters[name]<minimum || parameters[name]>maximum) {
        throw new Error(`CdA parameter ${name} must be between ${minimum} and ${maximum}`);
      }
    }
  }
  function calculate(watts,wind,parameters=defaults) {
    validate(parameters);
    if (!Number.isFinite(watts)) return {cda:null,reason:"missing_power"};
    if (!Number.isFinite(wind)) return {cda:null,reason:"missing_wind"};
    if (watts<=0) return {cda:null,reason:"no_power"};
    if (wind<3) return {cda:null,reason:"low_wind"};
    const aerodynamicPower=watts*parameters.efficiency-parameters.mass*9.80665*parameters.crr*wind;
    return aerodynamicPower>0
      ? {cda:aerodynamicPower/(0.5*parameters.rho*wind**3),reason:null}
      : {cda:null,reason:"nonpositive_aero"};
  }
  globalThis.aeroProbeEstimate=Object.freeze({defaults,validate,calculate});
})();
