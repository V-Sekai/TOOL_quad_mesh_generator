# Justfile for building and running quadwild on a Blender Monkey

clean:
    rm -rf build
    rm -rf output_with_boundary_*

build:
    cmake . -B build -DWITH_GUROBI=0 -DSATSUMA_ENABLE_BLOSSOM5=0
    cmake --build build --parallel

run:
    @just build
    ./build/Build/bin/quadwild ./output_with_boundary.obj 2 ./config/prep_config/basic_setup.txt
    ./build/Build/bin/quad_from_patches ./output_with_boundary_rem_p0.obj 1000 ./config/main_config/flow_noalign.txt
    open -a Preview output_with_boundary_rem_p0_1000_quadrangulation_smooth.obj
