/*
==============================================================================
V5 / ESP32-S3 cycling-computer enclosure / 2026-10-16 / units: mm
基于 aerodynamic_cycling_computer.scad 的可编辑重建，不是最终 STL 包装。

【源几何与使用方式】
1. 同目录 aerodynamic_cycling_computer_V5_sources.scad 仅保存原始壳体与扣座的
   顶点/面数据；由原始输入转换，不包含 V3/V4/最终修改件的网格。
2. 原始主板定位、接口、按钮和扣座仍由原始面数据定义；没有冒称已把这些
   原厂细节全部逆向为尺寸草图。新增/切除/曲线/壁厚/保护区在本文件定义。
3. OpenSCAD 打开本文件即可参数预览；F6 才是实体求交/合并。目前使用本地
    Docker 的 ARM64 OpenSCAD 2021.01；不升级宿主机 Blender。
    静态检查不等于 F6 或打印验收；每次修改应重新导出独立 STL 验证。
4. V5 是参数化设计重建，不承诺与 Blender 手工修面 STL 三角面逐点一致。
   尤其 V4 局部倒圆、原孔外接环和屏幕尾边挤出的边界需重新核对。

【修改过程回述 / 仅最后接受的状态进入默认总装】
S00 原稿：原壳 + 背部 52x36x2.5 电池仓 + 前置探头 + Garmin 转接座。
S01 探头：向屏幕倾斜 5 度；伸出 50；颈部收缩 6%；内孔直径 6.5。
S02 曾给尾端两个背孔补 3 mm 台阶与抬高环，后来明确要求恢复原样。
    V5 不调用原稿 matched_back_hole_steps()，最后恢复原件两个孔区。
S03 打印薄壁补强：正面边缘 +0.3、侧面 +0.4、接口槽底 +0.45、
    后壁轴向加厚 25%、尾端薄皮 +0.7。这里保留原网格顶点映射参数。
    这些是几何增量，不构成材料/工艺/整体刚性的保证。
S04 曾尝试重做一体背壳、单侧探头槽及背面流线形变；用户后续回到
    print_reinforced 为 V3 基准。已拒绝的整块背壳/形变不重新混入。
S05 电池仓内凹角 R1.0，属于补料圆角，会占用矩形电池的边角空间。
S06 探头前端球头 R5.3，最前端局部 Z=-50；内球 R3.25；
    屏幕侧 5x16 开槽，局部 Z=-47..-31；正前端封闭、下壁保留。
    原 Blender 导出在接头处用了 0.02 mm 半径避共面，此处明确保留。
    原槽缘还请求过 R0.3 bevel，但实际受网格拓扑限幅，不能当确定半径。
    V5 明确保留槽切体 R0.5；槽缘实际倒圆需在 F6 输出后重新评估。
S07 V4 外直角切圆/内角补料：尝试外 R0.4、内 R0.6，最终导出存在
    退化面，未获得整件打印认证。V5 将角部切体独立成模块，并保护孔位；
    不用全局 Minkowski 把螺孔、接口或转接座一并改形。
S08 GPS 曾讨论左侧，最终改为面对屏幕、探头朝下时的右侧，即局部 +Y。
    初始独立仓净空按前后 6、向右 10、机身方向 10 mm；后要求机身
    方向改到 20 mm，同时外凸限 5 mm。随后明确 20 为外形总长，
    留两端壁后的旧仓净长为 16.4；最终又取消独立净空，合并主板腔。
    依次尝试独立外扩仓、外凸 5、长度 20、实心、再挖空；均有历史记录，
    只有下面 S09/S10 的主板腔连通、齐平封顶版本进入最终设计。
S09 GPS 与主板腔整块相连，不保留独立小孔/隔板；底面约 X=2.45，
    尾段降至 X=1.2，跟随主板腔原台阶；朝屏幕顶壳厚 1.8 并齐平。
    最终不再保证一个 6x10x20 的独立矩形净空，天线尺寸需按截面复核。
S10 GPS 外侧：从 Z=1.8 渐扩至 Z=43.8；仓段原长 20，后半段
    Z=53.8..63.8 平滑收回。内壁沿同一曲线偏置挖空；尾端空间会变窄。
    最大侧向外凸按主侧面 Y=28.58 为基准限制到 5，而不是背部收窄面。
S11 两个尾端背孔以原始 obj_1..._3.stl 恢复孔径及补高，孔心局部
    Y=+-20.5,Z=69.35。Blender 最终用平面切分/环缝合而非失败的布尔。
    V5 用原孔实体交集恢复核心、在核心之外连接；不把失败结果当基准。
S12 用户纠正补强方向：是正面，不是背面；位置为远离探头的尾端横边；
    加厚量由 1 改为 2 mm。向 +X 加料，不侵占内部，不跨过屏幕开口。
S13 F6 外观复核后：删除探头根部旧 16x16x2 GPS 承托，避免管外方块；
    正面加强条靠机身的一侧由零高度斜坡接入，外沿仍保留最大 2 mm；
    迎风端前外缘斜切 1.2、后外缘斜切 0.6，避免头部直接直角迎风。
    内部定位面、探头孔、侧置 GPS 仓和恢复的两个背孔不随之改尺寸。
S14 左右侧与主体连接的四条纵向外棱切圆，名义 R1；正面四角按 R2
    收圆，切体贯穿到加高后正面。只减去外凸角的材料，不做整体缩放；
    GPS 流线实体及原孔区受保护，转接座和探头在主壳切圆后单独合并。

【历史验收边界】
最后人工检查 STL 仍有 29 张继承退化面；原孔 640 点对照曾通过，
GPS 曲线/连通及正面增厚有抽样记录，但不能继承为 V5 的实体认证。
V5 需独立执行 F6、STL 回读、各孔/接口/内腔/壁厚/自交检查。
这些局部曲面是基于历史参数的重建；未做原 STL 的盲目缩放、体素修复，
也未把 Blender 的源码检查或局部数值放样当成 OpenSCAD 的运行结果。
==============================================================================
*/

include <aerodynamic_cycling_computer_V5_sources.scad>

$fn = 96;
epsilon = 0.02;
view_stage = 12;             // 0=原壳, 3=补强, 5=电池仓, 6=探头, 7=角部, 10=GPS, 11=原孔, 12=正面条形补强
section_view = false;        // 仅用于查看内部；true 不是完整打印件

// 一、沿用原稿的世界坐标，不把观察视角的上下误当成模型轴。
case_center_y = 140.668;
case_center_z = 38.050;
case_back_x = 154.256;
case_front_x = 167.850;
case_top_z = 78.100;          // +Z 尾端；-Z 才是探头迎风端
case_angle_z = 91.238;
frame_angle = case_angle_z - 90;

// 二、独立特征开关。关闭某特征不表示恢复某个历史 STL 的全部面拓扑。
enable_wall_reinforcement = true;
enable_battery = true;
enable_probe = true;
enable_corner_rounding = true;
enable_gps = true;
enable_original_tail_holes = true;
enable_front_strip = true;
enable_windward_bevel = true;
enable_mount = true;

// 三、原壳薄壁补强。仅原厂壳顶点映射；新构件各自使用明确壁厚。
front_rim_extra = 0.3;
side_wall_extra = 0.4;
connector_floor_extra = 0.45;
back_depth_gain = 0.25;
tail_skin_extra = 0.7;

// 四、电池仓：保留原稿净空名义值，圆角后不能再声称可装同尺寸直角电池。
bat_cavity_l = 52;
bat_cavity_w = 36;
bat_cavity_h = 2.5;
wall_thickness = 1.8;
battery_inner_radius = 1;
fairing_steps = 24;
fairing_side_steps = 12;
fairing_side_run = 6.4;
fairing_overlap = 0.6;

// 五、探头局部坐标：原点在机身迎风端，局部 -Z 向前，+X 向屏幕。
pitot_outer_w = 10;
pitot_outer_th = 13.5;
pitot_inner_d = 6.5;
pitot_reach = 50;
pitot_angle = 5;
pitot_neck_reduction = 0.06;
pitot_root_overlap = 4;
pitot_slot_w = 5;
pitot_slot_d = 16;           // 原稿重复声明 15/16；V5 只保留最终值 16
pitot_slot_start = -47;
pitot_slot_corner = 0.5;
pitot_nose_radius = 5.3;
pitot_inner_joint_relief = 0.02;
gps_shelf_w = 16;            // 历史尺寸，仅记录；S13 已取消旧探头根部方形承托
gps_shelf_l = 16;
gps_shelf_t = 2;

// 六、孔位：不得再给尾端两个孔加同样的 3 mm 小孔/补高环。
back_hole_centers = [[120.194,9.35],[161.185,9.35],[120.194,69.35],[161.185,69.35]];
back_hole_clearance_r = 2.8;
tail_hole_side = 20.5;
tail_hole_z = 69.35;
tail_hole_core_half = 3.8;
tail_hole_guard_half = 4.5;

// 七、角部操作为参数化替代，不是 V4 的选择性三角网格 bevel。
outer_corner_increment = 1.0;
inner_corner_increment = 0.6;
case_outer_corner_radius = 1 + outer_corner_increment;
side_edge_radius = 1.0;
side_edge_steps = 160;
corner_cut_back_x = -1.2;
corner_cut_front_x = 17;     // 覆盖正面 2 mm 加强条，而非旧切体的 X=7..13

// 八、GPS 外侧曲线与内腔，全部使用机身局部坐标。
gps_wall = 1.8;
gps_side_datum = 28.58;
gps_side_projection = 5;
gps_outer_right = gps_side_datum + gps_side_projection;
gps_curve_start = 1.8;
gps_full_width_z = 43.8;
gps_nominal_length = 20;
gps_tail_start = gps_full_width_z + gps_nominal_length/2;
gps_tail_end = gps_full_width_z + gps_nominal_length;
gps_outer_left = 26.2;
gps_outer_back = -0.6;
gps_inner_left = 24.9;       // 越过原侧壁，整截面连入主板腔，不是单独走线孔
mainboard_floor = 2.45;
mainboard_tail_floor = 1.2;
mainboard_floor_step = 61.1;
gps_curve_steps = 160;
gps_edge_radius = 0.6;
gps_front_overlap = 0.005;   // 数值接缝余量，不是宣称绝对零误差的齐平
front_plane_point = [13.22547,26,53.8];
front_plane_normal_world = [0.99971807,0.02356208,-0.00292781];

// 九、正面尾端横条：正面是 +X，不是背面 -X。保留原内侧表面。
front_strip_extra = 2;
front_strip_start_z = 74.09;
front_strip_min_x = 13.28;
front_strip_ramp_start_z = 74.1;
front_strip_ramp_end_z = case_top_z; // 原始面片在尾沿结束；斜坡沿整条边宽线性接入
head_front_bevel = 1.2;
head_rear_bevel = 0.6;
head_bevel_run = 1.2;        // 小于原主板腔前端 3 mm 留边；不修改内部定位面

// 十、Garmin 配合本体不做圆角/缩放。保留原稿安装位置与姿态。
garmin_embed = 1;

function clamp01(value) = min(1,max(0,value));
function fairing_blend(value) = let(t=clamp01(value)) t*t*(3-2*t);
function curve_blend(value) = let(t=clamp01(value)) t*t*t*(10-15*t+6*t*t);
function to_local(point) = let(dx=point[0]-case_back_x,dy=point[1]-case_center_y)
    [dx*cos(frame_angle)+dy*sin(frame_angle),-dx*sin(frame_angle)+dy*cos(frame_angle),point[2]];
function to_world(point) = [case_back_x+point[0]*cos(frame_angle)-point[1]*sin(frame_angle),
    case_center_y+point[0]*sin(frame_angle)+point[1]*cos(frame_angle),point[2]];
function contains_value(value,values) = len(search(value,values))>0;
function strip_raise(point) = front_strip_extra*clamp01(
    (point[2]-front_strip_ramp_start_z)/(front_strip_ramp_end_z-front_strip_ramp_start_z));
function compact_polygon(vertices) = [for(index=[0:len(vertices)-1])
    if(vertices[index]!=vertices[(index+len(vertices)-1)%len(vertices)]) vertices[index]];
function front_strip_face(face,points) = let(normal=cross(points[face[1]]-points[face[0]],points[face[2]]-points[face[0]]))
    norm(normal)>epsilon*epsilon && abs(normal[0])/norm(normal)>0.9999
    && len([for(index=face) if(points[index][0]>front_strip_min_x && points[index][2]>front_strip_start_z) index])==len(face);

// 与 repair_print_walls.py 的原壳映射同源；保留未知原厂细节的拓扑。
function reinforced_local(point) = let(depth=point[0],side=point[1],length=point[2],
    body=fairing_blend(length/3),
    rim=front_rim_extra*fairing_blend((depth-11.99)/0.996),
    support=back_depth_gain*min(2,max(0,depth+0.013))*fairing_blend((5.5-depth)/3.5),
    tail=tail_skin_extra*fairing_blend((length-74.6)/1.5)*fairing_blend((0.6-depth)/0.6),
    recess=fairing_blend((side+27.4)/0.9)*fairing_blend((-25.5-side)/0.75)
        *fairing_blend((depth-1.5)/0.5)*fairing_blend((8.5-depth)/0.5)
        *fairing_blend((length-34)/1)*fairing_blend((53-length)/1.25),
    lateral=sign(side)*side_wall_extra*fairing_blend((abs(side)-27.4)/0.9)-connector_floor_extra*recess)
    length<0 ? point : [depth+(rim+support-tail)*body,side+lateral*body,length];
function base_local(point) = view_stage>=3 && enable_wall_reinforcement ? reinforced_local(to_local(point)) : to_local(point);
function front_height(side,length) = let(nx=front_plane_normal_world[0]*cos(frame_angle)+front_plane_normal_world[1]*sin(frame_angle),
    ny=-front_plane_normal_world[0]*sin(frame_angle)+front_plane_normal_world[1]*cos(frame_angle),nz=front_plane_normal_world[2])
    front_plane_point[0]-(ny*(side-front_plane_point[1])+nz*(length-front_plane_point[2]))/nx;
function gps_right(length) = length<=gps_full_width_z
    ? 28.45+(gps_outer_right-28.45)*curve_blend((length-gps_curve_start)/(gps_full_width_z-gps_curve_start))
    : length>gps_tail_start ? gps_outer_right+(28.45-gps_outer_right)*curve_blend((length-gps_tail_start)/(gps_tail_end-gps_tail_start))
    : gps_outer_right;
function gps_slope(length) = (gps_right(length+0.001)-gps_right(length-0.001))/0.002;
function gps_inner_right(length) = gps_right(length)-gps_wall*sqrt(1+gps_slope(length)*gps_slope(length));
function gps_floor(length) = mainboard_floor+(mainboard_tail_floor-mainboard_floor)
    *curve_blend((length-mainboard_floor_step+0.02)/0.04);
function gps_stations(lower,upper) = concat(
    [for(step=[0:gps_curve_steps]) let(length=lower+(upper-lower)*step/gps_curve_steps)
        if(length<mainboard_floor_step-0.02) length],
    [for(length=[mainboard_floor_step-0.02,mainboard_floor_step+0.02])
        if(length>=lower && length<=upper) length],
    [for(step=[0:gps_curve_steps]) let(length=lower+(upper-lower)*step/gps_curve_steps)
        if(length>mainboard_floor_step+0.02) length]);

module body_frame() {
    translate([case_back_x,case_center_y,0]) rotate([0,0,frame_angle]) children();
}
module probe_frame() {
    translate([(case_back_x+case_front_x)/2,case_center_y,0.5]) rotate([0,-pitot_angle,0]) children();
}

// 源面数据保留原始面朝向；OpenSCAD 的 polyhedron 使用外视顺时针面。
module base_case(original=false,strip=false) {
    local_points=[for(point=v5_case_points) original ? to_local(point) : base_local(point)];
    selected=strip ? [for(index=[0:len(v5_case_faces)-1]) if(front_strip_face(v5_case_faces[index],local_points)) index] : [];
    edges=[for(index=selected,step=[0:len(v5_case_faces[index])-1])
        [v5_case_faces[index][step],v5_case_faces[index][(step+1)%len(v5_case_faces[index])]]];
    boundary=[for(edge=edges) if(len([for(other=edges) if(other[0]==edge[1] && other[1]==edge[0]) 1])==0) edge];
    duplicates=[for(index=[0:len(local_points)-1])
        if(strip_raise(local_points[index])>0.000001
            && len([for(face_index=selected) if(contains_value(index,v5_case_faces[face_index])) 1])>0) index];
    function raised(index) = contains_value(index,duplicates) ? len(local_points)+search(index,duplicates)[0] : index;
    points=concat([for(point=local_points) to_world(point)],
        [for(index=duplicates) to_world(local_points[index]+[strip_raise(local_points[index]),0,0])]);
    faces=concat([for(index=[0:len(v5_case_faces)-1]) if(!contains_value(index,selected)) v5_case_faces[index]],
        [for(index=selected) [for(vertex=v5_case_faces[index]) raised(vertex)]],
        [for(edge=boundary) let(side=compact_polygon([edge[0],edge[1],raised(edge[1]),raised(edge[0])]))
            if(len(side)>=3) side]);
    polyhedron(points,faces,convexity=20);
}
module garmin_mount() {
    translate([case_back_x-bat_cavity_h-wall_thickness+garmin_embed,case_center_y,case_center_z])
        rotate([0,0,case_angle_z]) polyhedron(v5_mount_points,v5_mount_faces,convexity=10);
}

// 通用闭合截面放样：每圈点数一致、从 +Z 看逆时针；面索引按 SCAD 约定。
module ring_loft(rings) {
    count=len(rings[0]); rows=len(rings);
    polyhedron(points=[for(ring=rings) for(point=ring) point],
        faces=concat([[for(index=[0:count-1]) index]],
            [for(row=[0:rows-2],index=[0:count-1])
                [row*count+index,(row+1)*count+index,(row+1)*count+(index+1)%count,row*count+(index+1)%count]],
            [[for(index=[count-1:-1:0]) (rows-1)*count+index]]),convexity=20);
}

// 沿用原稿气动电池背壳的三次曲线，不引入被撤回的一体大方背壳。
function back_profile(length) = let(span=bat_cavity_l+2*wall_thickness,
    start=case_center_z-span/2,end=case_center_z+span/2)
    length<start ? fairing_blend(length/start) : length>end ? 1-fairing_blend((length-end)/(case_top_z-end)) : 1;
module protected_hole_zones(half=tail_hole_guard_half) {
    body_frame() for(side=[-tail_hole_side,tail_hole_side])
        translate([-8,side-half,tail_hole_z-half]) cube([13,2*half,2*half]);
}
module all_hole_clearances() {
    body_frame() for(hole=back_hole_centers)
        let(side=(hole[0]-case_center_y)/cos(frame_angle))
        translate([-12,side,hole[1]]) rotate([0,90,0]) cylinder(h=30,r=back_hole_clearance_r);
}
module battery_bulge_outer() {
    half=bat_cavity_w/2+wall_thickness;
    rings=[for(step=[0:fairing_steps*2]) let(length=epsilon+(case_top_z-2*epsilon)*step/(fairing_steps*2),
        rear=-(bat_cavity_h+wall_thickness)*back_profile(length))
        concat([for(index=[0:fairing_side_steps*2]) let(side=-half-fairing_side_run
            +2*(half+fairing_side_run)*index/(fairing_side_steps*2),
            blend=fairing_blend((half+fairing_side_run-abs(side))/fairing_side_run))
            [rear*blend,side,length]],
            [[fairing_overlap,half+fairing_side_run,length],[fairing_overlap,-half-fairing_side_run,length]])];
    difference() {
        body_frame() ring_loft([for(ring=rings) [for(index=[len(ring)-1:-1:0]) ring[index]]]);
        all_hole_clearances();
        protected_hole_zones();
    }
}
module rounded_box(size,radius) {
    assert(min(size)>2*radius,"Rounded box radius exceeds available size");
    hull() for(depth=[radius,size[0]-radius],side=[radius,size[1]-radius],length=[radius,size[2]-radius])
        translate([depth,side,length]) sphere(r=radius,$fn=32);
}
module battery_cavity_inner() {
    translate([case_back_x-bat_cavity_h,case_center_y-bat_cavity_w/2,case_center_z-bat_cavity_l/2])
        rounded_box([bat_cavity_h+5,bat_cavity_w,bat_cavity_l],battery_inner_radius);
}

// 椭圆放样与球头：尺寸来自 build_v3_probe_cap.py，而不是导入其结果 STL。
function probe_radii(length) = let(neck=1-pitot_neck_reduction*pow(sin(180*clamp01(-length/pitot_reach)),2),
    root=fairing_blend((length+10)/14)) [pitot_outer_th/2*neck+0.15*root,pitot_outer_w/2*neck+3.5*root];
module pitot_loft(sections) {
    ring_loft([for(section=sections) [for(index=[0:$fn-1])
        [section[1]*cos(360*index/$fn),section[2]*sin(360*index/$fn),section[0]]]]);
}
module probe_outer_local() {
    center=-pitot_reach+pitot_nose_radius;
    translate([0,0,center]) sphere(r=pitot_nose_radius);
    pitot_loft([for(step=[0:64]) let(length=center-0.1+(-31.4-center+0.1)*step/64,
        blend=fairing_blend((length-center)/5.7),old=probe_radii(length),
        overlap=0.08*fairing_blend((length+35)/2))
        [length,pitot_nose_radius+(old[0]-pitot_nose_radius)*blend+overlap,
         pitot_nose_radius+(old[1]-pitot_nose_radius)*blend+overlap]]);
    pitot_loft([for(step=[0:72]) let(length=-31.5+(31.5+pitot_root_overlap)*step/72,radii=probe_radii(length))
        [length,radii[0],radii[1]]]);
}
module pitot_bore_cutter() {
    radius=pitot_inner_d/2;
    center=-pitot_reach+pitot_nose_radius;
    probe_frame() union() {
        translate([0,0,center]) sphere(r=radius);
        pitot_loft([[center-0.1,radius,radius],[-33.2,radius,radius],
            [-32.8,radius-pitot_inner_joint_relief,radius-pitot_inner_joint_relief],
            [-30.8,radius-pitot_inner_joint_relief,radius-pitot_inner_joint_relief]]);
        pitot_loft(concat([[-31,radius,radius],[-8,radius,radius]],
            [for(step=[1:28]) let(length=-8+step*0.5,wide=radius+fairing_blend((length+8)/14)) [length,wide,wide]],
            [[15,radius+1,radius+1]]));
    }
}
module probe_slot_cutter() {
    probe_frame() translate([0,-pitot_slot_w/2,pitot_slot_start])
        rounded_box([15,pitot_slot_w,pitot_slot_d],pitot_slot_corner);
}
module forward_pitot_tube() {
    difference() {
        probe_frame() probe_outer_local();
        pitot_bore_cutter();
        probe_slot_cutter();
    }
}

// 四个 Y/Z 角贯穿整个主壳深度，包含正面外缘与加高后的尾端横条。
module shell_corner_cutters() {
    radius=case_outer_corner_radius;
    body_frame() for(side_sign=[-1,1],end=[0,case_top_z])
        translate([(corner_cut_back_x+corner_cut_front_x)/2,
                   side_sign*(28.7-radius),end==0 ? radius : case_top_z-radius])
            rotate([0,90,0]) linear_extrude(height=corner_cut_front_x-corner_cut_back_x,center=true)
                difference() {
                    translate([end==0 ? 0 : -radius-epsilon,side_sign<0 ? -radius-epsilon : 0])
                        square([radius+epsilon,radius+epsilon]);
                    circle(r=radius);
                }
}

// 左/右侧各有前外棱与后外棱，共四条。切体跟随原壳前平面和尾端补强斜坡。
function side_edge_section(length,side_sign,front) = let(radius=side_edge_radius,
    front_sign=front ? 1 : -1,
    edge_x=front ? front_height(side_sign*28.7,length)
        + (view_stage>=12 && enable_front_strip ? strip_raise([0,0,length]) : 0)
        : -0.013-tail_skin_extra*fairing_blend((length-74.6)/1.5),
    edge_y=side_sign*(28.3+side_wall_extra*fairing_blend(length/3)),
    center=[edge_x-front_sign*radius,edge_y-side_sign*radius],
    outline=concat([[radius+epsilon,0],[radius+epsilon,radius+epsilon],[0,radius+epsilon]],
        [for(step=[0:24]) [radius*cos(90-step*90/24),radius*sin(90-step*90/24)]]),
    ring=[for(point=outline) [center[0]+front_sign*point[0],center[1]+side_sign*point[1],length]])
    front_sign*side_sign>0 ? ring : [for(index=[len(ring)-1:-1:0]) ring[index]];
module side_edge_cutters() {
    difference() {
        body_frame() union() for(side_sign=[-1,1],front=[false,true])
            ring_loft([for(step=[0:side_edge_steps])
                side_edge_section(-epsilon+(case_top_z+2*epsilon)*step/side_edge_steps,side_sign,front)]);
        // GPS 接合处不是外角，不在内部已封顶曲面上挖出纵向沟槽。
        if(view_stage>=10 && enable_gps) gps_flow_outer();
        protected_hole_zones();
    }
}

// 迎风端仅修外缘：+X 侧与补强后的前表面衔接，-X 侧去掉方锐边。
// 2D 坐标为局部 X/Z，沿 Y 拉伸；探头在外壳差集之后单独合并。
module windward_edge_cutters() {
    front_at_end=12.988+front_rim_extra*fairing_blend(head_bevel_run/3);
    body_frame() translate([0,35,0]) rotate([90,0,0]) linear_extrude(height=70) union() {
        polygon([[12.988-head_front_bevel,-epsilon],[18,-epsilon],
                 [18,head_bevel_run+epsilon],[front_at_end+epsilon,head_bevel_run+epsilon]]);
        polygon([[-15,-epsilon],[head_rear_bevel,-epsilon],
                 [-epsilon,head_bevel_run+epsilon],[-15,head_bevel_run+epsilon]]);
    }
}

// 主板腔纵向凹角采用补料圆角，不削薄原壁。+Y GPS 开仓在其后扣除。
module mainboard_inner_fillets() {
    difference() {
        body_frame() union() for(side_sign=[-1,1],front=[false,true])
            let(corner_x=front ? 11.988 : 2.488,side=side_sign*25.3,radius=inner_corner_increment,
                start=3,end=front ? 76.1 : 61.1)
            translate([corner_x,side,start]) scale([front ? -1 : 1,-side_sign,1])
                linear_extrude(height=end-start) difference() {
                    translate([-epsilon,-epsilon]) square([radius+epsilon,radius+epsilon]);
                    translate([radius,radius]) circle(r=radius);
                }
        protected_hole_zones();
    }
}

// GPS 截面沿 Z 放样。前后均使用五次 smoothstep，接入处一阶、二阶斜率为零。
function gps_section(length,inner=false) = let(left=inner ? gps_inner_left : gps_outer_left,
    right=inner ? gps_inner_right(length) : gps_right(length),rear=inner ? gps_floor(length) : gps_outer_back,
    front=front_plane_point[0]-(inner ? gps_wall : 0),radius=gps_edge_radius)
    inner ? [[front_height(right,length)-gps_wall,right,length],[rear,right,length],
        [rear,left,length],[front_height(left,length)-gps_wall,left,length]]
    : [for(corner=[[front-radius,right-radius,0],[rear+radius,right-radius,90],
                  [rear+radius,left+radius,180],[front-radius,left+radius,270]],step=[0:8])
        let(angle=corner[2]+step*90/8,depth=corner[0]+radius*cos(angle),side=corner[1]+radius*sin(angle),
            fraction=(depth-rear)/(front-rear))
        [depth+fraction*(front_height(side,length)-front_plane_point[0]+gps_front_overlap),side,length]];
module gps_flow_outer() {
    body_frame() ring_loft([for(length=gps_stations(gps_curve_start,gps_tail_end)) gps_section(length)]);
}
module gps_flow_cavity() {
    // 两端各留 1.8 mm。主板侧切到 Y=24.9，取消独立仓隔板。
    body_frame() ring_loft([for(length=gps_stations(gps_curve_start+gps_wall,gps_tail_end-gps_wall))
        gps_section(length,true)]);
}

// 原孔核心最后恢复，防止后续开仓/圆角/补强再修改原孔径及补高。
module restore_original_tail_holes() {
    union() {
        difference() { children(); protected_hole_zones(tail_hole_core_half); }
        intersection() { base_case(original=true); protected_hole_zones(tail_hole_core_half+epsilon); }
    }
}

module main_shell(strip=false) {
    difference() {
        union() {
            base_case(strip=strip);
            if(view_stage>=5 && enable_battery) battery_bulge_outer();
            if(view_stage>=7 && enable_corner_rounding) mainboard_inner_fillets();
            if(view_stage>=10 && enable_gps) gps_flow_outer();
        }
        if(view_stage>=5 && enable_battery) battery_cavity_inner();
        if(view_stage>=10 && enable_gps) gps_flow_cavity();
        if(view_stage>=7 && enable_corner_rounding) shell_corner_cutters();
        if(view_stage>=7 && enable_corner_rounding) side_edge_cutters();
        if(view_stage>=6 && enable_windward_bevel) windward_edge_cutters();
    }
}
module complete_aerodynamic_cycling_computer() {
    difference() {
        union() {
            if(view_stage>=11 && enable_original_tail_holes)
                restore_original_tail_holes() main_shell(view_stage>=12 && enable_front_strip);
            else main_shell(view_stage>=12 && enable_front_strip);
            if(view_stage>=6 && enable_probe) forward_pitot_tube();
            if(view_stage>=5 && enable_mount) garmin_mount();
        }
        // 与原稿一致：最后贯通探头孔，但不穿过球头的封闭前端。
        if(view_stage>=6 && enable_probe) pitot_bore_cutter();
    }
}

// 参数门槛只证明设计输入合理，不替代实体渲染/实测壁厚。
assert(view_stage>=0 && view_stage<=12,"view_stage must be 0..12");
assert(gps_tail_end<66.1,"GPS shell must stop before the tail button region");
assert(gps_wall>0 && gps_wall<3,"Invalid GPS wall thickness");
assert(gps_inner_right(gps_tail_end-gps_wall)>gps_inner_left,"Rear curve closes GPS interior");
assert(tail_hole_core_half>2.5,"Original 5 mm hole core must fit protection zone");
assert(front_strip_extra>=0 && front_strip_extra<=3,"Check screen-front reinforcement height");
assert(front_strip_ramp_end_z>front_strip_ramp_start_z,"Invalid front strip ramp span");
assert(head_bevel_run>0 && head_bevel_run<3,"Windward bevel must not reach mainboard cavity");
assert(side_edge_radius>0 && side_edge_radius<=1.2,"Long-edge radius must preserve rim and interface walls");
assert(case_outer_corner_radius>=side_edge_radius && case_outer_corner_radius<=2.5,"Check four front-corner radius");

if(section_view)
    intersection() {
        complete_aerodynamic_cycling_computer();
        body_frame() translate([-20,-40,-60]) cube([40,40,150]);
    }
else complete_aerodynamic_cycling_computer();