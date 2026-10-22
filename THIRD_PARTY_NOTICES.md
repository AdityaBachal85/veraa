# Third-party software in Veraa

Veraa uses the following open-source components. Each is used under its own license.

| Component | Use in Veraa | License | Source |
|---|---|---|---|
| Qt 6 (Qt Base: Core, Gui, Widgets, OpenGL) | Desktop user interface | LGPL v3 (dynamically linked) | https://www.qt.io/ |
| Intel Embree 4 | Ray tracing acceleration | Apache License 2.0 | https://github.com/RenderKit/embree |
| Intel Open Image Denoise 2 | Denoiser | Apache License 2.0 | https://github.com/RenderKit/oidn |
| oneAPI Threading Building Blocks (TBB) | Used by Embree and Open Image Denoise | Apache License 2.0 | https://github.com/oneapi-src/oneTBB |
| nlohmann/json 3.11.3 | Scene files | MIT | https://github.com/nlohmann/json |
| stb_image / stb_image_write (Sean Barrett) | Texture loading, image saving | Public domain / MIT | https://github.com/nothings/stb |
| Microsoft Visual C++ runtime (Windows builds) | C++ runtime DLLs | Microsoft Visual Studio redistributable terms | https://learn.microsoft.com/cpp/windows/redistributing-visual-cpp-files |
| Sample HDRI from Poly Haven | Sample scenes | CC0 | https://polyhaven.com/ |

Qt is linked dynamically, so you can replace the Qt DLLs shipped with Veraa with your own
build of the same Qt version. Qt's source code is available from https://download.qt.io/.

Veraa is an independent project. It is inspired by the workflows of Autodesk 3ds Max and
Chaos V-Ray but contains none of their code and is not affiliated with Autodesk or Chaos.
3ds Max is a trademark of Autodesk, Inc. V-Ray is a trademark of Chaos Software.
