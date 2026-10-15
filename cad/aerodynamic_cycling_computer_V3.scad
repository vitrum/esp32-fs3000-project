$fn = 96;

module reinforced_base() {
    import("aerodynamic_cycling_computer_print_reinforced.stl", convexity = 10);
}

module aerodynamic_cycling_computer_v3() {
    reinforced_base();
}

aerodynamic_cycling_computer_v3();