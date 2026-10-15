$fn = 96;
show_rounded_battery = true;
show_spherical_probe = true;

module reinforced_base() {
    import("aerodynamic_cycling_computer_print_reinforced.stl", convexity = 10);
}

module rounded_battery_base() {
    import("aerodynamic_cycling_computer_V3_rounded_battery.stl", convexity = 10);
}

module spherical_probe_base() {
    import("aerodynamic_cycling_computer_V3_spherical_probe.stl", convexity = 10);
}

module aerodynamic_cycling_computer_v3() {
    if (show_spherical_probe)
        spherical_probe_base();
    else if (show_rounded_battery)
        rounded_battery_base();
    else
        reinforced_base();
}

aerodynamic_cycling_computer_v3();