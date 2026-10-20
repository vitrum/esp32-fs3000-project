/* FIT Activity encoder for the browser model; field layout matches firmware. */
(() => {
  "use strict";
  window.encodeRideFit = (rows, startUtc) => {
    if (!rows.length || !Number.isInteger(startUtc) || startUtc < 1577836800)
      throw new Error("FIT requires samples and trusted UTC");
    const data = [];
    const epoch = 631065600;
    const num = (value, size = 1) => {
      for (let i = 0; i < size; i++) data.push((value >>> (i * 8)) & 255);
    };
    const definition = (local, global, fields, developer = false) => {
      num(0x40 | local | (developer ? 0x20 : 0)); num(0); num(0); num(global, 2); num(fields.length);
      fields.forEach((field) => field.forEach((value) => num(value)));
      if (developer) { num(5); for (let i = 0; i < 5; i++) { num(i); num(i === 4 ? 1 : 4); num(0); } }
    };
    const text = (value, length) => { for (let i = 0; i < length; i++) num(i < value.length ? value.charCodeAt(i) : 0); };
    const float = (value) => {
      const bytes = new Uint8Array(4);
      new DataView(bytes.buffer).setFloat32(0, value, true);
      bytes.forEach((byte) => num(byte));
    };
    const start = startUtc - epoch;
    const elapsed = rows[rows.length - 1][0] - rows[0][0];
    const end = start + Math.floor(elapsed / 1000);
    let distance = 0;
    const distances = rows.map((row, i) => {
      if (i) distance += (row[3] + rows[i - 1][3]) * .5 * (row[0] - rows[i - 1][0]) / 1000;
      return Math.round(distance * 100);
    });
    definition(0, 0, [[0,1,0],[1,2,0x84],[4,4,0x86]]);
    num(0); num(4); num(255,2); num(start,4);
    definition(1,207,[[1,16,0x0D],[3,1,2]]);
    num(1); text("AeroProbeFIT",12); num(0); num(0); num(0); num(1); num(0);
    definition(2,206,[[0,1,2],[1,1,2],[2,1,2],[3,24,7],[8,8,7]]);
    ["airspeed","yaw","posture","air_density","power_source"].forEach((name,i) => {
      num(2); num(0); num(i); num(i === 4 ? 2 : 0x88); text(name,24); text(["m/s","deg","deg","kg/m3",""][i],8);
    });
    definition(3,21,[[253,4,0x86],[0,1,0],[1,1,0]]);
    const event = (timestamp,type) => { num(3); num(timestamp,4); num(0); num(type); };
    event(start,0);
    definition(4,20,[[253,4,0x86],[7,2,0x84],[4,1,2],[6,2,0x84],[5,4,0x86]],true);
    rows.forEach((row,i) => {
      num(4); num(start + Math.floor((row[0] - rows[0][0]) / 1000),4);
      num(row[8] ? Math.round(row[1]) : 0xFFFF,2); num(row[8] ? Math.round(row[2]) : 0xFF);
      num(Math.min(0xFFFE, Math.round(row[3] * 1000)),2); num(distances[i],4);
      row.slice(4,8).forEach(float); num(row[8]);
    });
    event(end,4);
    for (const lap of [true,false]) {
      const local = lap ? 5 : 6;
      const fields = [[253,4,0x86],[0,1,0],[1,1,0],[2,4,0x86],[7,4,0x86],[8,4,0x86],[9,4,0x86],[lap ? 25 : 5,1,0]];
      if (!lap) fields.push([6,1,0],[25,2,0x84],[26,2,0x84]);
      definition(local, lap ? 19 : 18,fields);
      num(local); num(end,4); num(lap ? 9 : 8); num(1); num(start,4);
      num(elapsed,4); num(elapsed,4); num(distances[distances.length-1],4); num(2);
      if (!lap) { num(0); num(0,2); num(1,2); }
    }
    definition(7,34,[[253,4,0x86],[0,4,0x86],[1,2,0x84],[2,1,0],[3,1,0],[4,1,0]]);
    num(7); num(end,4); num(elapsed,4); num(1,2); num(0); num(26); num(1);
    const crc = (bytes) => {
      let value = 0;
      for (const byte of bytes) {
        value ^= byte;
        for (let i = 0; i < 8; i++) value = value & 1 ? (value >>> 1) ^ 0xA001 : value >>> 1;
      }
      return value;
    };
    const header = [14,0x20,0x34,0x08,...[0,8,16,24].map((shift) => (data.length >>> shift) & 255),46,70,73,84];
    const headerCrc = crc(header);
    header.push(headerCrc & 255,headerCrc >>> 8);
    const bytes = new Uint8Array(header.length + data.length + 2);
    bytes.set(header); bytes.set(data,14);
    const fileCrc = crc(bytes.subarray(0,bytes.length-2));
    bytes.set([fileCrc & 255,fileCrc >>> 8],bytes.length-2);
    return bytes;
  };
})();
