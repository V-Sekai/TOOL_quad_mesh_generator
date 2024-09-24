# Justfile for building and running quadwild

build_quadwild:
    cmake . -B build -DWITH_GUROBI=0 -DSATSUMA_ENABLE_BLOSSOM5=1
    cd build && make -j

run_quadwild:
    cd Build/bin
    ./quadwild.exe ./mesh.ply 2 ./config/prep_config/basic_setup.txt
    ./quad_from_patches.exe ./mesh_rem_p0.obj 1000 ./config/main_config/flow_noalign.txt
