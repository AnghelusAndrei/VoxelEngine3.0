# VoxelEngine3.0
```
This is my 3rd voxel engine, this one is made using OpenGL and GLFW and highly optimised for the GPU.
Succesfully implements:
-Sparse Voxel Octrees
-A 4 pass raytracing rendering system:
  -RayPass (fragment):
    -finds the intersection with the closest voxel (DDA on octrees optimised with bitwise operations)
    -calculates the incoming light contribution (custom raytracing)
    -repeats for every light bounce and sample
    -stores a color image and a voxel ID image
  -AccumPass (compute):
    -Accumulates color for pixels sharing the same voxel ID in a spatial and temporal buffer using complex Atomic operations 
    -stores the amount of pixels which hit the same voxel in the buffer for every pixel proccesed currently
    -updates previous data from older frames
  -AvgPass (compute):
    -Averages color for pixels sharing the same voxel ID (using Atomic Operations)
  -RenderPass (vertex + fragment):
    -renders the final image to the scene
-per voxel normals
-simple material system
```

## To do:
```
-update structure to a more complex double tree mip-mapping cache system capable of LOD and loading huge amounts of voxels into RAM at runtime  
-Integrate ReSTIR with the current spatio-temporal Atomic buffer approach
-Object system(import, move and rotate voxel structures, textures and more)
```

# Build:

## Windows:

### Requires:
```
-MinGW C++ compiler
-Cmake 3.11
```

```
git clone https://github.com/AnghelusAndrei/VoxelEngine3.0.git
cd VoxelEngine3.0
mkdir build
cd build
cmake -G "MinGW Makefiles" ..
cmake --build .
```
