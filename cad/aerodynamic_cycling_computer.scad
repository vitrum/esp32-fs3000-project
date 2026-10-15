// ==============================================================================
// 智能码表 / ESP32-S3 显示外壳气动结构改造脚本 (OpenSCAD)
// 包含：
// 1. 前置 FS3000 风速传感器中空仰角探头管 (带有 5mm 装配插槽、中空走线通道、内部 GPS 天线固定台阶)
// 2. 背部电池仓扩展腔体 (外扩净空 52 x 36 x 2.5 mm，需核对电池厚度)
// 3. 气动倒角收腰过渡与佳明 Garmin 卡扣 (gamin-block.stl) 背对背贴合
// 4. 原始壳体 (obj_1_esp32-s3-touch-lcd-2_8_3 copy.stl) 一体化融合
// ==============================================================================

$fn = 60; // 渲染圆滑度

// ------------------------------
// 一、基础全局坐标对齐参数 (与原 STL 保持坐标系统一)
// ------------------------------
// 原外壳几何范围：X: [153.63, 167.85], Y: [112.37, 169.24], Z: [0, 78.10]
// 码表方向：-Z 为探头迎风端，+Z 为尾端；+X 为屏幕正面，-X 为外壳背面；Y 为左右对称轴
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
pitot_angle     = 5.0;
pitot_neck_reduction = 0.06;
pitot_root_overlap = 4.0;
pitot_slot_w    = 5.0;    // FS3000 电路板竖插槽宽度 5.0 mm
pitot_slot_d    = 15.0;   // 插槽深度 15 mm
pitot_slot_d    = 16.0;   // 插槽深度 16 mm (留足板卡卡固与迎风探头伸出空间)
gps_shelf_w     = 16.0;   // GPS 天线台阶宽度 (适配 15x15 mm / 18x18 mm 贴片)
gps_shelf_l     = 16.0;   // GPS 天线台阶长度
gps_shelf_t     = 2.0;    // GPS 天线承托厚度

// 2. 背部电池仓扩展腔体参数 (外扩净空 52 x 36 x 2.5 mm)
bat_cavity_l    = 52.0;   // 电池仓内部净长 (Z轴方向，预留装配公差)
bat_cavity_w    = 36.0;   // 电池仓内部净宽 (Y轴方向)
bat_cavity_h    = 2.5;    // 外扩净深再减薄 5 mm；机身内侧挖槽深度保持不变
wall_thickness  = 1.8;    // 结构推荐壁厚 1.8 mm (满足 3D 打印强度要求)
garmin_embed    = 1.0;    // 卡扣底座嵌入凸台 1 mm，避免仅边缘搭接
fairing_steps  = 24;
fairing_overlap = 0.6;
fairing_side_steps = 12;
fairing_side_run = 6.4;
back_hole_radius = 2.8;
back_hole_step_depth = 0.987;
back_hole_bore_length = 1.0;
back_hole_bore_radius = 1.5;
back_hole_counterbore_radius = 2.5;
back_hole_support_radius = 3.3;
back_hole_centers = [
    [120.194, 9.350], [161.185, 9.350],
    [120.194, 69.350], [161.185, 69.350]
];

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
function fairing_blend(fraction) =
    let(clamped = min(1, max(0, fraction)))
    clamped * clamped * (3 - 2 * clamped);

function back_surface_x(position_y) =
    case_back_x - tan(case_angle_z - 90) * (position_y - case_center_y);

module back_hole_clearance() {
    for (hole = back_hole_centers)
        translate([back_surface_x(hole[0]), hole[0], hole[1]])
            rotate([0, 0, case_angle_z - 90])
                rotate([0, 90, 0])
                    cylinder(h = 30, r = back_hole_radius, center = true);
}

module matched_back_hole_steps() {
    for (hole = back_hole_centers)
        if (hole[1] > case_center_z)
            translate([back_surface_x(hole[0]), hole[0], hole[1]])
                rotate([0, 0, case_angle_z - 90])
                    rotate([0, 90, 0])
                        rotate_extrude($fn = 96)
                            polygon([
                                [back_hole_support_radius, back_hole_step_depth - 0.2],
                                [back_hole_support_radius, back_hole_step_depth + back_hole_bore_length],
                                [back_hole_bore_radius, back_hole_step_depth + back_hole_bore_length],
                                [back_hole_bore_radius, back_hole_step_depth],
                                [back_hole_counterbore_radius, back_hole_step_depth],
                                [back_hole_counterbore_radius, back_hole_step_depth - 0.2]
                            ]);
}

module battery_bulge_outer() {
    l = bat_cavity_l + 2 * wall_thickness;
    w = bat_cavity_w + 2 * wall_thickness;
    h = bat_cavity_h + wall_thickness;
    platform_start = case_center_z - l / 2;
    platform_end = case_center_z + l / 2;
    leading_profile = [for (step = [0 : fairing_steps])
        let(fraction = step / fairing_steps)
        [platform_start * fraction,
         h * fraction * fraction * (3 - 2 * fraction)]];
    trailing_profile = [for (step = [0 : fairing_steps])
        let(fraction = step / fairing_steps)
        [platform_end + (case_top_z - platform_end) * fraction,
         h * (1 - fraction * fraction * (3 - 2 * fraction))]];
    outer_profile = concat(leading_profile, trailing_profile);

    side_positions = concat(
        [for (step = [0 : fairing_side_steps])
            case_center_y - w / 2 - fairing_side_run * (1 - step / fairing_side_steps)],
        [for (step = [0 : fairing_side_steps])
            case_center_y + w / 2 + fairing_side_run * step / fairing_side_steps]
    );
    columns = len(side_positions);
    rows = len(outer_profile);
    layer_size = columns * rows;
    vertices = [for (layer = [0 : 1], row = [0 : rows - 1], column = [0 : columns - 1])
        let(position_y = side_positions[column],
            back_x = back_surface_x(position_y),
            side_blend = fairing_blend((w / 2 + fairing_side_run - abs(position_y - case_center_y)) / fairing_side_run),
            depth_blend = outer_profile[row][1] / h)
        [layer == 0
            ? back_x - (back_x - (case_back_x - h)) * side_blend * depth_blend
            : back_x + fairing_overlap,
         position_y, outer_profile[row][0]]];
    surface_faces = [for (row = [0 : rows - 2], column = [0 : columns - 2], triangle = [0 : 3])
        let(index = row * columns + column)
        triangle == 0 ? [index, index + columns + 1, index + 1] :
        triangle == 1 ? [index, index + columns, index + columns + 1] :
        triangle == 2 ? [index + layer_size, index + 1 + layer_size, index + columns + 1 + layer_size] :
                       [index + layer_size, index + columns + 1 + layer_size, index + columns + layer_size]];
    perimeter = concat(
        [for (column = [0 : columns - 1]) column],
        [for (row = [1 : rows - 1]) row * columns + columns - 1],
        [for (column = [columns - 2 : -1 : 0]) (rows - 1) * columns + column],
        [for (row = [rows - 2 : -1 : 1]) row * columns]
    );
    edge_faces = [for (index = [0 : len(perimeter) - 1])
        let(start = perimeter[index], finish = perimeter[(index + 1) % len(perimeter)])
        [start, finish, finish + layer_size, start + layer_size]];
    difference() {
        polyhedron(vertices, concat(surface_faces, edge_faces), convexity = 10);
        back_hole_clearance();
    }
}

// 4. 背部电池仓内部净空挖槽 (外扩净深 2.5 mm，向码表内侧延伸)
module battery_cavity_inner() {
    cavity_start_x = case_back_x - bat_cavity_h;
    translate([cavity_start_x, case_center_y - bat_cavity_w/2, case_center_z - bat_cavity_l/2]) {
        cube([bat_cavity_h + 5, bat_cavity_w, bat_cavity_l]);
    }
}

module pitot_bore_cutter() {
    root_x = (case_back_x + case_front_x) / 2.0;
    inner_sections = concat(
        [[-55, 3.25, 3.25], [-8, 3.25, 3.25]],
        [for (step = [1 : 28])
            let(position_z = -8 + step * 0.5,
                radius = pitot_inner_d / 2 + fairing_blend((position_z + 8) / 14))
            [position_z, radius, radius]],
        [[15, 4.25, 4.25]]
    );
    translate([root_x, case_center_y, 0.5])
        rotate([0, -pitot_angle, 0])
            pitot_loft(inner_sections);
}

function pitot_outer_radii(position_z) =
    let(fraction = min(1, max(0, -position_z / pitot_reach)),
        neck = 1 - pitot_neck_reduction * pow(sin(180 * fraction), 2),
        root_blend = fairing_blend((position_z + 10) / 14),
        tip_distance = max(0, -49.2 - position_z),
        tip_round = 0.8 - sqrt(max(0, pow(0.8, 2) - pow(tip_distance, 2))))
    [pitot_outer_th / 2 * neck + 0.15 * root_blend - tip_round,
     pitot_outer_w / 2 * neck + 3.5 * root_blend - tip_round];

module pitot_loft(sections) {
    segments = 96;
    points = [for (section = sections, segment = [0 : segments - 1])
        [section[1] * cos(360 * segment / segments),
         section[2] * sin(360 * segment / segments), section[0]]];
    side_faces = [for (ring = [0 : len(sections) - 2], segment = [0 : segments - 1])
        let(start = ring * segments + segment,
            finish = ring * segments + (segment + 1) % segments)
        [start, finish, finish + segments, start + segments]];
    polyhedron(points, concat(
        [[for (segment = [segments - 1 : -1 : 0]) segment]],
        side_faces,
        [[for (segment = [0 : segments - 1]) (len(sections) - 1) * segments + segment]]
    ), convexity = 10);
}

// 5. 前置气动风速计探头管 (移至另一端面 Z=0，带 FS3000 插槽与 GPS 安装台阶)
module forward_pitot_tube() {
    // 根部精准定位在外壳另一侧端面中央 (Z = 0.0 mm)
    root_x = (case_back_x + case_front_x) / 2.0;
    root_y = case_center_y;
    root_z = 0.0;

    outer_sections = [for (step = [0 : 108])
        let(position_z = -pitot_reach + step * 0.5,
            radii = pitot_outer_radii(position_z))
        [position_z, radii[0], radii[1]]];
    translate([root_x, root_y, root_z + 0.5]) rotate([0, -pitot_angle, 0]) {
        difference() {
            // [实体] 沿前向延伸 50 mm 的水滴流线型柱体 (外宽 10mm，厚 13.5mm)
            union() {
                pitot_loft(outer_sections);
                
                // 内部侧 GPS 天线安装支承台阶
                translate([-gps_shelf_w/2, -gps_shelf_l/2, -5.0])
                    cube([gps_shelf_w, gps_shelf_l, gps_shelf_t]);
            }

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
                    matched_back_hole_steps();
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
