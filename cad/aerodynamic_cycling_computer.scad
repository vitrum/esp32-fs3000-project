// ==============================================================================
// 智能码表 / ESP32-S3 显示外壳气动结构改造脚本 (OpenSCAD)
// 包含：
// 1. 前置 FS3000 风速传感器中空仰角探头管 (带有 5mm 装配插槽、中空走线通道、内部 GPS 天线固定台阶)
// 2. 背部电池仓扩展腔体 (净空 52 x 36 x 7.5 mm，容纳锂电池)
// 3. 气动倒角收腰过渡与佳明 Garmin 卡扣 (gamin-block.stl) 背对背贴合
// 4. 原始壳体 (obj_1_esp32-s3-touch-lcd-2_8_3 copy.stl) 一体化融合
// ==============================================================================

$fn = 60; // 渲染圆滑度

// ------------------------------
// 一、基础全局坐标对齐参数 (与原 STL 保持坐标系统一)
// ------------------------------
// 原外壳几何范围：X: [153.63, 167.85], Y: [112.37, 169.24], Z: [0, 78.10]
// 码表方向：+Z 为前端迎风头部，-Z 为按键尾部；+X 为屏幕正面，-X 为外壳背面；Y 为左右对称轴
case_center_y  = 140.668; // 宽度中轴 Y 坐标
case_center_z  = 38.050;  // 垂直高度中轴 Z 坐标
case_back_x    = 154.256; // 背部平面的基准 X 坐标
case_front_x   = 167.850; // 正面屏幕平面的基准 X 坐标
case_top_z     = 78.100;  // 前端最上沿 Z 坐标
case_angle_z   = 91.238;  // 背面微调角度

// ------------------------------
// 二、结构控制参数 (全部参数化可调)
// ------------------------------

// 1. 前置风速计探头 (Pitot Tube) 参数
pitot_outer_w   = 10.0;   // 迎风外径宽度 10 mm (1 cm)
pitot_outer_th  = 13.5;   // 顺风向水滴弦长 13.5 mm (与外壳厚度完美吻合)
pitot_inner_d   = 6.5;    // 中空中孔内径 6.5 mm (用于信号走线)
pitot_reach     = 50.0;   // 探头最远端距离码表前部表面精准 5 厘米 (50 mm)
pitot_slot_w    = 5.0;    // FS3000 电路板竖插槽宽度 5.0 mm
pitot_slot_d    = 15.0;   // 插槽深度 15 mm
pitot_slot_d    = 16.0;   // 插槽深度 16 mm (留足板卡卡固与迎风探头伸出空间)
gps_shelf_w     = 16.0;   // GPS 天线台阶宽度 (适配 15x15 mm / 18x18 mm 贴片)
gps_shelf_l     = 16.0;   // GPS 天线台阶长度
gps_shelf_t     = 2.0;    // GPS 天线承托厚度

// 2. 背部电池仓扩展腔体参数 (内部净空 52 x 36 x 7.5 mm)
bat_cavity_l    = 52.0;   // 电池仓内部净长 (Z轴方向，预留装配公差)
bat_cavity_w    = 36.0;   // 电池仓内部净宽 (Y轴方向)
bat_cavity_h    = 7.5;    // 电池仓内部净深 (较原设计缩小 3 mm)
wall_thickness  = 1.8;    // 结构推荐壁厚 1.8 mm (满足 3D 打印强度要求)
garmin_embed    = 1.0;    // 卡扣底座嵌入凸台 1 mm，避免仅边缘搭接

// ------------------------------
// 三、功能子模块定义
// ------------------------------

// 1. 导入原始外壳
module base_case() {
    color("LightSteelBlue", 0.9)
    import("obj_1_esp32-s3-touch-lcd-2_8_3 copy.stl", convexity = 5);
}

// 2. 佳明通用卡扣 (gamin-block.stl)
module garmin_mount(extrude_x = 0) {
    color("Coral")
    translate([case_back_x - bat_cavity_h - wall_thickness + garmin_embed, case_center_y, case_center_z])
    rotate([0, 0, case_angle_z])
    import("gamin-block.stl", convexity = 5);
}

// 3. 背部电池仓外部隆起流线型壳体
module battery_bulge_outer() {
    l = bat_cavity_l + 2 * wall_thickness;
    w = bat_cavity_w + 2 * wall_thickness;
    h = bat_cavity_h + wall_thickness;
    
    // 从外壳背面 (-X 方向) 向后凸出并做气动收锥和圆角
    translate([case_back_x - h, case_center_y - w/2, case_center_z - l/2]) {
        hull() {
            // 底部贴合根部
            translate([h - 0.5, 0, 0])
                cube([0.5, w, l]);
            
            // 后部安装平台 (稍微内收形成气动锥度并倒角)
            translate([0, 2.0, 2.0])
                cube([1.0, w - 4.0, l - 4.0]);
        }
    }
}

// 4. 背部电池仓内部净空挖槽 (净深 7.5 mm，向码表内侧延伸)
module battery_cavity_inner() {
    cavity_start_x = case_back_x - bat_cavity_h;
    translate([cavity_start_x, case_center_y - bat_cavity_w/2, case_center_z - bat_cavity_l/2]) {
        cube([bat_cavity_h + 5, bat_cavity_w, bat_cavity_l]);
    }
}

module pitot_bore_cutter() {
    root_x = (case_back_x + case_front_x) / 2.0;
    translate([root_x, case_center_y, -pitot_reach - 5])
        cylinder(h = pitot_reach + 20, d = pitot_inner_d, center = false);
}

// 5. 前置气动风速计探头管 (移至另一端面 Z=0，带 FS3000 插槽与 GPS 安装台阶)
module forward_pitot_tube() {
    // 根部精准定位在外壳另一侧端面中央 (Z = 0.0 mm)
    root_x = (case_back_x + case_front_x) / 2.0;
    root_y = case_center_y;
    root_z = 0.0;

    translate([root_x, root_y, root_z]) {
        difference() {
            // [实体] 沿前向延伸 50 mm 的水滴流线型柱体 (外宽 10mm，厚 13.5mm)
            union() {
                translate([0, 0, -pitot_reach])
                    scale([pitot_outer_th / 10.0, pitot_outer_w / 10.0, 1.0])
                    cylinder(h = pitot_reach, d = 10.0, center = false);
                
                // 内部侧 GPS 天线安装支承台阶
                translate([-gps_shelf_w/2, -gps_shelf_l/2, -5.0])
                    cube([gps_shelf_w, gps_shelf_l, gps_shelf_t]);
            }

            // [扣除 1] 中空贯通走线孔 (直通壳体内部)
            translate([0, 0, -pitot_reach - 5])
                cylinder(h = pitot_reach + 10, d = pitot_inner_d, center = false);

            // [扣除 2] 最远端开设 5mm 竖向直插槽 (用于垂直安装 FS3000 电路板)
            translate([-pitot_outer_th, -pitot_slot_w/2, -pitot_reach - 1])
                cube([pitot_outer_th * 2, pitot_slot_w, pitot_slot_d + 1]);
        }
    }
}

// ------------------------------
// 四、整体布尔融合总装
// ------------------------------
module complete_aerodynamic_cycling_computer() {
    difference() {
        union() {
            // 1. 只在外壳与凸起中向机身内侧挖仓，保留外壁和卡扣贴合面
            difference() {
                union() {
                    base_case();
                    battery_bulge_outer();
                }
                battery_cavity_inner();
            }

            // 2. 融合前置风速传感器探头管
            forward_pitot_tube();

            // 3. 背部最外层贴合佳明卡扣底座
            garmin_mount();
        }

        // 只对总装贯穿扣除探头走线孔
        pitot_bore_cutter();
    }
}

// 渲染总成
complete_aerodynamic_cycling_computer();
