![image](https://github.com/user-attachments/assets/a9490327-ebea-4086-9ff0-3b72658fab3a)# VoxelEngine3.0
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

# Images
### Raytracing
![image](https://github.com/user-attachments/assets/0b2da2ea-1eb6-4dfd-adfb-e517fd563a5d)
### Structure
![image](https://github.com/user-attachments/assets/7f2b0241-a7a2-4d2b-bf52-8a2d17f07a6e)
### Albedo
![image](https://github.com/user-attachments/assets/0eabc2e8-b4bd-4705-9486-b4e34ce7797a)
### Normal
![image](https://github.com/user-attachments/assets/1dd7b0a3-8eee-45f0-8f7c-d90cc333fca4)
### Voxel ID
![image](https://github.com/user-attachments/assets/3e0087e9-dcb2-4297-9dd4-77006ab3bf4e)


### Extreme example (10.452.410 voxels in a scene)
![image](https://github.com/user-attachments/assets/b2aafa31-d550-44fe-9f88-e9fee321ce05)
![image](https://github.com/user-attachments/assets/a403150c-f39b-499a-b884-dd5a2aab2afd)


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
