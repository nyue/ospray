// ======================================================================== //
// Copyright 2016 SURVICE Engineering Company                               //
// Copyright 2009-2016 Intel Corporation                                    //
//                                                                          //
// Licensed under the Apache License, Version 2.0 (the "License");          //
// you may not use this file except in compliance with the License.         //
// You may obtain a copy of the License at                                  //
//                                                                          //
//     http://www.apache.org/licenses/LICENSE-2.0                           //
//                                                                          //
// Unless required by applicable law or agreed to in writing, software      //
// distributed under the License is distributed on an "AS IS" BASIS,        //
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied. //
// See the License for the specific language governing permissions and      //
// limitations under the License.                                           //
// ======================================================================== //

#include "imgui3D.h"

#include <imgui.h>
#include "imgui_impl_glfw_gl3.h"

#include "ospcommon/utility/CodeTimer.h"
#include "ospcommon/utility/getEnvVar.h"
#include "ospray/version.h"

#include <stdio.h>
#include <GLFW/glfw3.h>

#ifdef _WIN32
#  define snprintf(buf,len, format,...) _snprintf_s(buf, len,len, format, __VA_ARGS__)
#  ifndef WIN32_LEAN_AND_MEAN
#    define WIN32_LEAN_AND_MEAN
#  endif
#  define _USE_MATH_DEFINES
#  include <math.h> // M_PI
#else
#  include <sys/times.h>
#endif

#include <fstream>
#include <sstream>
#include <stdexcept>
#include <string>

extern "C" void glDrawPixels( GLsizei width, GLsizei height,
                              GLenum format, GLenum type,
                              const GLvoid *pixels );

using ospcommon::utility::getEnvVar;

namespace ospray {
  namespace imgui3D {

    bool dumpScreensDuringAnimation = false;
    bool ImGui3DWidget::showGui = false;

    static ImGui3DWidget *currentWidget = nullptr;

    // Class definitions //////////////////////////////////////////////////////

    /*! write given frame buffer to file, in PPM P6 format. */
    void saveFrameBufferToFile(const char *fileName,
                               const uint32_t *pixel,
                               const uint32_t sizeX, const uint32_t sizeY)
    {
      FILE *file = fopen(fileName,"wb");
      if (!file) {
        std::cerr << "#osp:glut3D: Warning - could not create screenshot file '"
                  << fileName << "'" << std::endl;
        return;
      }
      fprintf(file,"P6\n%i %i\n255\n",sizeX,sizeY);
      unsigned char *out = (unsigned char *)alloca(3*sizeX);
      for (size_t y = 0; y < sizeY; y++) {
        const unsigned char *in =
            (const unsigned char *)&pixel[(sizeY-1-y)*sizeX];
        for (size_t x = 0; x < sizeX; x++) {
          out[3*x+0] = in[4*x+0];
          out[3*x+1] = in[4*x+1];
          out[3*x+2] = in[4*x+2];
        }
        fwrite(out, 3*sizeX, sizeof(char), file);
      }
      fprintf(file,"\n");
      fclose(file);
      std::cout << "#osp:glut3D: saved framebuffer to file "
                << fileName << std::endl;
    }

    /*! currently active window */
    ImGui3DWidget *ImGui3DWidget::activeWindow = nullptr;
    vec2i ImGui3DWidget::defaultInitSize(1024,768);

    bool ImGui3DWidget::animating = false;

    // InspectCenter Glut3DWidget::INSPECT_CENTER;
    /*! viewport as specified on the command line */
    ImGui3DWidget::ViewPort *viewPortFromCmdLine = nullptr;

    // ------------------------------------------------------------------
    // implementation of glut3d::viewPorts
    // ------------------------------------------------------------------
    ImGui3DWidget::ViewPort::ViewPort() :
      modified(true),
      from(0,0,-1),
      at(0,0,0),
      up(0,1,0),
      openingAngle(60.f),
      aspect(1.f),
      apertureRadius(0.f)
    {
      frame = AffineSpace3fa::translate(from) * AffineSpace3fa(ospcommon::one);
    }

    void ImGui3DWidget::ViewPort::snapViewUp()
    {
      auto look = at - from;
      auto right = cross(look, up);
      up = normalize(cross(right, look));
    }

    void ImGui3DWidget::ViewPort::snapFrameUp()
    {
      if (fabsf(dot(up,frame.l.vz)) < 1e-3f)
        return;
      frame.l.vx = normalize(cross(frame.l.vy,up));
      frame.l.vz = normalize(cross(frame.l.vx,frame.l.vy));
      frame.l.vy = normalize(cross(frame.l.vz,frame.l.vx));
    }

    void ImGui3DWidget::motion(const vec2i &pos)
    {
      currMousePos = pos;
      if (!renderingPaused) manipulator->motion(this);
      lastMousePos = currMousePos;
    }

    void ImGui3DWidget::mouseButton(int button, int action, int mods)
    {}

    ImGui3DWidget::ImGui3DWidget(FrameBufferMode frameBufferMode,
                                 ManipulatorMode initialManipulator,
                                 int allowedManipulators) :
      lastMousePos(-1,-1),
      currMousePos(-1,-1),
      windowSize(-1,-1),
      motionSpeed(.003f),
      rotateSpeed(.003f),
      frameBufferMode(frameBufferMode),
      fontScale(2.f),
      ucharFB(nullptr),
	  moveModeManipulator(nullptr),
	  inspectCenterManipulator(nullptr)
    {
      if (activeWindow != nullptr)
        throw std::runtime_error("ERROR: Can't create more than one ImGui3DWidget!");

      activeWindow = this;

      worldBounds.lower = vec3f(-1);
      worldBounds.upper = vec3f(+1);

      if (allowedManipulators & INSPECT_CENTER_MODE) {
        inspectCenterManipulator = new InspectCenter(this);
      }
      if (allowedManipulators & MOVE_MODE) {
        moveModeManipulator = new MoveMode(this);
      }
      switch(initialManipulator) {
      case MOVE_MODE:
        manipulator = moveModeManipulator;
        break;
      case INSPECT_CENTER_MODE:
        manipulator = inspectCenterManipulator;
        break;
      }
      Assert2(manipulator != nullptr,"invalid initial manipulator mode");

      if (viewPortFromCmdLine) {
        viewPort = *viewPortFromCmdLine;

        if (length(viewPort.up) < 1e-3f)
          viewPort.up = vec3f(0,0,1.f);

        this->worldBounds = worldBounds;
        computeFrame();
      }

      currButton[0] = currButton[1] = currButton[2] = GLFW_RELEASE;

      displayTime=-1.f;
      renderTime=-1.f;
      guiTime=-1.f;
      totalTime=-1.f;
    }

    void ImGui3DWidget::computeFrame()
    {
      viewPort.frame.l.vy = normalize(viewPort.at - viewPort.from);
      viewPort.frame.l.vx = normalize(cross(viewPort.frame.l.vy,viewPort.up));
      viewPort.frame.l.vz = normalize(cross(viewPort.frame.l.vx,viewPort.frame.l.vy));
      viewPort.frame.p    = viewPort.from;
      viewPort.snapFrameUp();
      viewPort.modified = true;
    }

    void ImGui3DWidget::setZUp(const vec3f &up)
    {
      viewPort.up = up;
      if (up != vec3f(0.f))
        viewPort.snapFrameUp();
    }

    void ImGui3DWidget::reshape(const vec2i &newSize)
    {
      windowSize = newSize;
      viewPort.aspect = newSize.x/float(newSize.y);
    }

    void ImGui3DWidget::display()
    {
      if (animating) {
        auto *hack =
            (InspectCenter*)ImGui3DWidget::activeWindow->inspectCenterManipulator;
        hack->rotate(-.01f * ImGui3DWidget::activeWindow->motionSpeed, 0);
      }


      if (frameBufferMode == ImGui3DWidget::FRAMEBUFFER_UCHAR && ucharFB) {
        glDrawPixels(windowSize.x, windowSize.y,
                     GL_RGBA, GL_UNSIGNED_BYTE, ucharFB);
#ifndef _WIN32
        if (ImGui3DWidget::animating && dumpScreensDuringAnimation) {
          char tmpFileName[] = "/tmp/ospray_scene_dump_file.XXXXXXXXXX";
          static const char *dumpFileRoot;
          if (!dumpFileRoot) {
            auto rc = mkstemp(tmpFileName);
            (void)rc;
            dumpFileRoot = tmpFileName;
          }

          char fileName[100000];
          snprintf(fileName,sizeof(fileName),"%s_%08ld.ppm",dumpFileRoot,times(nullptr));
          saveFrameBufferToFile(fileName,ucharFB,windowSize.x,windowSize.y);
        }
#endif
      } else if (frameBufferMode == ImGui3DWidget::FRAMEBUFFER_FLOAT && floatFB) {
        glDrawPixels(windowSize.x, windowSize.y, GL_RGBA, GL_FLOAT, floatFB);
      } else {
        glClearColor(0.f,0.f,0.f,1.f);
        glClear( GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT );
      }
    }

    void ImGui3DWidget::buildGui()
    {
    }

    void ImGui3DWidget::setViewPort(const vec3f from,
                                    const vec3f at,
                                    const vec3f up)
    {
      const vec3f dir = at - from;
      viewPort.at     = at;
      viewPort.from   = from;
      viewPort.up     = up;

      this->worldBounds = worldBounds;
      viewPort.frame.l.vy = normalize(dir);
      viewPort.frame.l.vx = normalize(cross(viewPort.frame.l.vy,up));
      viewPort.frame.l.vz = normalize(cross(viewPort.frame.l.vx,viewPort.frame.l.vy));
      viewPort.frame.p    = from;
      viewPort.snapFrameUp();
      viewPort.modified = true;
    }

    void ImGui3DWidget::setWorldBounds(const box3f &worldBounds)
    {
      vec3f center = ospcommon::center(worldBounds);
      vec3f diag   = worldBounds.size();
      diag         = max(diag,vec3f(0.3f*length(diag)));
      vec3f from   = center - .75f*vec3f(-.6*diag.x,-1.2f*diag.y,.8f*diag.z);
      vec3f dir    = center - from;
      vec3f up     = viewPort.up;

      if (!viewPortFromCmdLine) {
        viewPort.at   = center;
        viewPort.from = from;
        viewPort.up   = up;

        if (length(up) < 1e-3f)
          up = vec3f(0,0,1.f);

        this->worldBounds = worldBounds;
        viewPort.frame.l.vy = normalize(dir);
        viewPort.frame.l.vx = normalize(cross(viewPort.frame.l.vy,up));
        viewPort.frame.l.vz = normalize(cross(viewPort.frame.l.vx,viewPort.frame.l.vy));
        viewPort.frame.p    = from;
        viewPort.snapFrameUp();
        viewPort.modified = true;
      }

      motionSpeed = length(diag) * .001f;
    }

    void ImGui3DWidget::setTitle(const std::string &title)
    {
      Assert2(window != nullptr,
              "must call Glut3DWidget::create() before calling setTitle()");
      glfwSetWindowTitle(window, title.c_str());
    }

    void ImGui3DWidget::create(const char *title, bool fullScreen)
    {
      // Setup window
      auto error_callback = [](int error, const char* description) {
        fprintf(stderr, "Error %d: %s\n", error, description);
      };

      glfwSetErrorCallback(error_callback);

      if (!glfwInit())
        throw std::runtime_error("Could not initialize glfw!");

      glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 2);
      glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 0);

      auto size = defaultInitSize;

      auto defaultSizeFromEnv =
          getEnvVar<std::string>("OSPRAY_APPS_DEFAULT_WINDOW_SIZE");

      if (defaultSizeFromEnv) {
        int rc = sscanf(defaultSizeFromEnv.value().c_str(),
                        "%dx%d", &size.x, &size.y);
        if (rc != 2) {
          throw std::runtime_error("could not parse"
                                   " OSPRAY_APPS_DEFAULT_WINDOW_SIZE "
                                   "env-var. Must be of format <X>x<Y>x<>Z "
                                   "(e.g., '1024x768'");
        }
      }

      if (fullScreen) {
        auto *monitor = glfwGetPrimaryMonitor();
        const GLFWvidmode* mode = glfwGetVideoMode(monitor);
        if(mode == nullptr) {
          throw std::runtime_error("could not get video mode");
        }
        window = glfwCreateWindow(mode->width, mode->height,
                                  title, monitor, nullptr);
      }
      else
        window = glfwCreateWindow(size.x, size.y, title, nullptr, nullptr);

      glfwMakeContextCurrent(window);

      // NOTE(jda) - move key handler registration into this class
      ImGui_ImplGlfwGL3_Init(window, true);

      glfwSetCursorPosCallback(
        window,
        [](GLFWwindow*, double xpos, double ypos) {
          ImGuiIO& io = ImGui::GetIO();
          if (!io.WantCaptureMouse)
            ImGui3DWidget::activeWindow->motion(vec2i(xpos, ypos));
        }
      );

      glfwSetMouseButtonCallback(
        window,
        [](GLFWwindow*, int button, int action, int mods) {
          ImGui3DWidget::activeWindow->currButton[button] = action;
          ImGui3DWidget::activeWindow->mouseButton(button, action, mods);
        }
      );

      glfwSetCharCallback(
        window,
        [](GLFWwindow*, unsigned int c) {
          ImGuiIO& io = ImGui::GetIO();
          if (c > 0 && c < 0x10000)
            io.AddInputCharacter((unsigned short)c);

          if (!io.WantCaptureKeyboard)
            ImGui3DWidget::activeWindow->keypress(c);
        }
      );

      currentWidget = this;
    }

    void run()
    {
      if (!currentWidget)
        throw std::runtime_error("ImGuiViewer window not created/set!");

      auto *window = currentWidget->window;

      int display_w = 0, display_h = 0;

      ImFontConfig config;
      config.MergeMode = false;

      #if 0 // NOTE(jda) - this can cause crashes in Debug builds, needs fixed
      ImGuiIO& io = ImGui::GetIO();
      ImFont* font =
          io.Fonts->AddFontFromFileTTF("LibreBaskerville-Regular.ttf",
                                       28, &config);
      if (font) {
        std::cout << "loaded font\n";
        currentWidget->fontScale = 1.f;
      }
      #endif

      // Main loop
      while (!glfwWindowShouldClose(window)) {
        ospcommon::utility::CodeTimer timer;
        ospcommon::utility::CodeTimer timerTotal;

        timerTotal.start();
        glfwPollEvents();
        timer.start();
        ImGui_ImplGlfwGL3_NewFrame();
        timer.stop();

        if (ImGui3DWidget::showGui)
          currentWidget->buildGui();

        ImGui::SetNextWindowPos(ImVec2(10,10));
        auto overlayFlags = ImGuiWindowFlags_NoTitleBar |
                            ImGuiWindowFlags_NoResize   |
                            ImGuiWindowFlags_NoMove     |
                            ImGuiWindowFlags_NoSavedSettings;
        bool open;
        if (!ImGui::Begin("Example: Fixed Overlay", &open,
                          ImVec2(0,0), 0.f, overlayFlags)) {
          ImGui::End();
        } else {
          ImFont* font = ImGui::GetFont();
          ImGui::PushStyleColor(ImGuiCol_Text, ImColor(.3,.3,.3,1.f));
          ImGui::SetWindowFontScale(currentWidget->fontScale*1.0f);
          font->Scale = 5.f;
          ImGui::Text("%s", ("OSPRay v" + std::string(OSPRAY_VERSION)).c_str());
          font->Scale = 1.f;
          ImGui::SetWindowFontScale(currentWidget->fontScale*0.7f);
          ImGui::PopStyleColor(1);

          std::stringstream ss;
          ss << 1.f/currentWidget->renderTime;
          ImGui::PushStyleColor(ImGuiCol_Text, ImColor(.8,.8,.8,1.f));
          ImGui::Text("%s", ("fps: " + ss.str()).c_str());
          ImGui::Text("press \'g\' for menu");
          ImGui::PopStyleColor(1);

          ImGui::End();
        }

        timer.start();
        int new_w = 0, new_h = 0;
        glfwGetFramebufferSize(window, &new_w, &new_h);

        if (new_w != display_w || new_h != display_h) {
          display_w = new_w;
          display_h = new_h;
          currentWidget->reshape(vec2i(display_w, display_h));
        }

        glViewport(0, 0, new_w, new_h);
        timer.stop();

        timer.start();
        // Render OSPRay frame
        currentWidget->display();
        timer.stop();
        currentWidget->displayTime = timer.secondsSmoothed();

        timer.start();
        // Render GUI
        ImGui::Render();
        timer.stop();
        currentWidget->guiTime = timer.secondsSmoothed();

        glfwSwapBuffers(window);
        timerTotal.stop();
        currentWidget->totalTime = timerTotal.secondsSmoothed();
      }

      // Cleanup
      ImGui_ImplGlfwGL3_Shutdown();
      glfwTerminate();
    }

    void init(int32_t *ac, const char **av)
    {
      for(int i = 1; i < *ac;i++) {
        std::string arg(av[i]);
        if (arg == "-win") {
          std::string arg2(av[i+1]);
          size_t pos = arg2.find("x");
          if (pos != std::string::npos) {
            arg2.replace(pos, 1, " ");
            std::stringstream ss(arg2);
            ss >> ImGui3DWidget::defaultInitSize.x
               >> ImGui3DWidget::defaultInitSize.y;
            removeArgs(*ac,(char **&)av,i,2); --i;
          } else {
            ImGui3DWidget::defaultInitSize.x = atoi(av[i+1]);
            ImGui3DWidget::defaultInitSize.y = atoi(av[i+2]);
            removeArgs(*ac,(char **&)av,i,3); --i;
          }
          continue;
        } if (arg == "--1k" || arg == "-1k") {
          ImGui3DWidget::defaultInitSize.x =
              ImGui3DWidget::defaultInitSize.y = 1024;
          removeArgs(*ac,(char **&)av,i,1); --i;
          continue;
        } if (arg == "--size") {
          ImGui3DWidget::defaultInitSize.x = atoi(av[i+1]);
          ImGui3DWidget::defaultInitSize.y = atoi(av[i+2]);
          removeArgs(*ac,(char **&)av,i,3); --i;
          continue;
        } if (arg == "-v" || arg == "--view") {
          std::ifstream fin(av[i+1]);
          if (!fin.is_open())
          {
            throw std::runtime_error("Failed to open \"" +
                                     std::string(av[i+1]) +
                                     "\" for reading");
          }

          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;

          auto& fx = viewPortFromCmdLine->from.x;
          auto& fy = viewPortFromCmdLine->from.y;
          auto& fz = viewPortFromCmdLine->from.z;

          auto& ax = viewPortFromCmdLine->at.x;
          auto& ay = viewPortFromCmdLine->at.y;
          auto& az = viewPortFromCmdLine->at.z;

          auto& ux = viewPortFromCmdLine->up.x;
          auto& uy = viewPortFromCmdLine->up.y;
          auto& uz = viewPortFromCmdLine->up.z;

          auto& fov = viewPortFromCmdLine->openingAngle;

          auto token = std::string("");
          while (fin >> token) {
            if (token == "-vp")
              fin >> fx >> fy >> fz;
            else if (token == "-vu")
              fin >> ux >> uy >> uz;
            else if (token == "-vi")
              fin >> ax >> ay >> az;
            else if (token == "-fv")
              fin >> fov;
            else {
              throw std::runtime_error("Unrecognized token:  \"" + token +
                                       '\"');
            }
          }

          assert(i+1 < *ac);
          removeArgs(*ac,(char **&)av, i, 2); --i;
          continue;
        } if (arg == "-vu") {
          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;
          viewPortFromCmdLine->up.x = atof(av[i+1]);
          viewPortFromCmdLine->up.y = atof(av[i+2]);
          viewPortFromCmdLine->up.z = atof(av[i+3]);
          assert(i+3 < *ac);
          removeArgs(*ac,(char **&)av,i,4); --i;
          continue;
        } if (arg == "-vp") {
          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;
          viewPortFromCmdLine->from.x = atof(av[i+1]);
          viewPortFromCmdLine->from.y = atof(av[i+2]);
          viewPortFromCmdLine->from.z = atof(av[i+3]);
          assert(i+3 < *ac);
          removeArgs(*ac,(char **&)av,i,4); --i;
          continue;
        } if (arg == "-vi") {
          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;
          viewPortFromCmdLine->at.x = atof(av[i+1]);
          viewPortFromCmdLine->at.y = atof(av[i+2]);
          viewPortFromCmdLine->at.z = atof(av[i+3]);
          assert(i+3 < *ac);
          removeArgs(*ac,(char **&)av,i,4); --i;
          continue;
        } if (arg == "-fv") {
          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;
          viewPortFromCmdLine->openingAngle = atof(av[i+1]);
          assert(i+1 < *ac);
          removeArgs(*ac,(char **&)av,i,2); --i;
          continue;
        } if (arg == "-ar") {
          if (!viewPortFromCmdLine)
            viewPortFromCmdLine = new ImGui3DWidget::ViewPort;
          viewPortFromCmdLine->apertureRadius = atof(av[i+1]);
          assert(i+1 < *ac);
          removeArgs(*ac,(char **&)av,i,2); --i;
          continue;
        }
      }
    }

    void ImGui3DWidget::keypress(char key)
    {
      switch (key) {
      case '!': {
        if (ImGui3DWidget::animating) {
          dumpScreensDuringAnimation = !dumpScreensDuringAnimation;
        } else {
          char tmpFileName[] = "/tmp/ospray_screen_dump_file.XXXXXXXX";
          static const char *dumpFileRoot;
#ifndef _WIN32
          if (!dumpFileRoot) {
            auto rc = mkstemp(tmpFileName);
            (void)rc;
            dumpFileRoot = tmpFileName;
          }
#endif
          char fileName[100000];
          static int frameDumpSequenceID = 0;
          snprintf(fileName, sizeof(fileName), "%s_%05d.ppm",dumpFileRoot,frameDumpSequenceID++);
          if (ucharFB)
            saveFrameBufferToFile(fileName,ucharFB,windowSize.x,windowSize.y);
          return;
        }

        break;
      }
      case 'C':
        PRINT(viewPort);
        break;
      case 'g':
        showGui = !showGui;
        break;
      case 'I':
        manipulator = inspectCenterManipulator;
        break;
      case 'M':
      case 'F':
        manipulator = moveModeManipulator;
        break;
      case 'A':
        ImGui3DWidget::animating = !ImGui3DWidget::animating;
        break;
      case '+':
        motionSpeed *= 1.5f;
        break;
      case '-':
        motionSpeed /= 1.5f;
        break;
      case 27 /*ESC*/:
      case 'q':
      case 'Q':
        std::exit(0);
        break;
      default:
        break;
      }
    }

    std::ostream &operator<<(std::ostream &o, const ImGui3DWidget::ViewPort &cam)
    {
      o << "// "
        << " -vp " << cam.from.x << " " << cam.from.y << " " << cam.from.z
        << " -vi " << cam.at.x << " " << cam.at.y << " " << cam.at.z
        << " -vu " << cam.up.x << " " << cam.up.y << " " << cam.up.z
        << std::endl;
      o << "<viewPort>" << std::endl;
      o << "  <from>" << cam.from.x << " " << cam.from.y << " " << cam.from.z << "</from>" << std::endl;
      o << "  <at>" << cam.at.x << " " << cam.at.y << " " << cam.at.z << "</at>" << std::endl;
      o << "  <up>" << cam.up.x << " " << cam.up.y << " " << cam.up.z << "</up>" << std::endl;
      o << "  <aspect>" << cam.aspect << "</aspect>" << std::endl;
      o << "  <frame.dx>" << cam.frame.l.vx << "</frame.dx>" << std::endl;
      o << "  <frame.dy>" << cam.frame.l.vy << "</frame.dy>" << std::endl;
      o << "  <frame.dz>" << cam.frame.l.vz << "</frame.dz>" << std::endl;
      o << "  <frame.p>" << cam.frame.p << "</frame.p>" << std::endl;
      o << "</viewPort>";
      return o;
    }
  }
}

