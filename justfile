# Justfile for building and running quadwild on a Blender Monkey

clean:
    rm -rf build
    rm -rf blender_monkey_*

build:
    cmake . -B build -DWITH_GUROBI=0 -DSATSUMA_ENABLE_BLOSSOM5=0
    cmake --build build --parallel

run:
    @just build
    ./build/Build/bin/quadwild ./blender_monkey.ply 2 ./config/prep_config/basic_setup.txt
    ./build/Build/bin/quad_from_patches ./blender_monkey_rem_p0.obj 1000 ./config/main_config/flow_noalign.txt
    open -a Preview blender_monkey_rem_p0_1000_quadrangulation_smooth.obj
