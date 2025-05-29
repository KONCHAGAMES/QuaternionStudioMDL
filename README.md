# **QuaternionStudioMDL**  

A modified version of **`studiomdl.exe`** based on **Valve's Source SDK (2018)**, replacing traditional Pitch/Yaw/Roll (Euler) angles with **raw quaternions** to prevent gimbal lock and preserve animation accuracy.  

> [!WARNING]
> This tool was modified **for personal use** and shared as-is.  
> **If you decide to use it, you'll need to adapt it for your own needs.**  


## **Features**  
- **Only RAW data** – Writing only raw data from .smd, without RLE and other funcs. You can modify it to suit your needs.  
- **Quaternion-Based Rotations** – Uses quaternions (xyzw) instead of Euler angles (pyr) to avoid gimbal lock and maintain precise orientation.  
- **Direct struct** `mstudio_frame_anim_t` instead `mstudio_rle_anim_t` for uncompressed, more reliable animation data.  
- **Skip RLE Compression**  

<br>

> [**Why Quaternions?**]
> - No singularities or axis flips (unlike Euler angles).  
> - Smooth interpolation for animations.  
> - Better preservation of 3D rotations.  

<br>

## **Old SMD Struct (BoneID XYZ PYR)**
```
1 -0.3777 -10.989902 -3.29355 3.1415926535898 -1.5707949125185 3.1415926535898
```

## **New SMD Struct** (BoneID XYZ XYZW)
```
1 -0.3777 -10.989902 -3.29355 0.0 -0.707107 0.0 0.707106
```
