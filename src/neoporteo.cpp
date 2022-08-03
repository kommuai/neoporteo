#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include <GL/glew.h>

#include <SDL.h>
#include <SDL_opengl.h>

#include <mpv/client.h>
#include <mpv/render_gl.h>

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "imnodes.h"

#include <bzlib.h>
#include <capnp/common.h>
#include <capnp/schema-parser.h>
#include <capnp/serialize-packed.h>
#include <capnp/dynamic.h>
#include <kj/array.h>

struct Dataset {
  std::vector<double> value;
  std::vector<double> time;
};

struct GraphItem {
  bool reset = true;
  std::string name;
  Dataset *dataset;
};

std::vector<Dataset> datasets;
GraphItem *cur_g = nullptr;

struct NodeMeta {
  std::string name;
  std::vector<std::string> ins;
  std::vector<std::string> outs;
};

struct NodeNY1XPlot {
  bool reset = true;
  std::vector<double> *x;
  std::vector<std::string> ynames;
  std::vector<std::vector<double> *> ys;
};

struct NodeData {
  std::string name;
  std::vector<double> *x;
  std::vector<double> *y;
};

NodeMeta node_dict[2] = {
  {
    .name = "NY1X Line Plot",
    .ins = {"x", "y"},
    .outs = {},
  },
  {
    .name = "Data",
    .ins = {},
    .outs = {"x", "y"},
  },
};

struct Node {
  int base;
  int type;
  union {
    NodeNY1XPlot *plot;
    NodeData *data;
  } n;
};

std::unordered_map<int, struct Node*> nodes;
int nodec = 0;

std::vector<std::pair<int, int>> links;

class SchemaItem {
  std::string guiid;
public:
  bool empty;
  std::string name;
  std::vector<SchemaItem> children;
  int dataset_id;

  SchemaItem(capnp::StructSchema::Field f)
  {
    name = f.getProto().getName();
    guiid = "si_" + name;

    if (f.getType().which() != capnp::schema::Type::STRUCT) {
      dataset_id = datasets.size();
      datasets.push_back(Dataset());
      return;
    }

    for (const auto& field : f.getType().asStruct().getFields()) {
      auto child = field.getProto().getName();
      if (child.endsWith("DEPRECATED"))
        continue;
      auto t = field.getType().which();
      switch (t) {
      case capnp::schema::Type::STRUCT:
      case capnp::schema::Type::FLOAT32:
      case capnp::schema::Type::FLOAT64:
      case capnp::schema::Type::INT8:
      case capnp::schema::Type::INT16:
      case capnp::schema::Type::INT32:
      case capnp::schema::Type::INT64:
      case capnp::schema::Type::UINT8:
      case capnp::schema::Type::UINT16:
      case capnp::schema::Type::UINT32:
      case capnp::schema::Type::UINT64:
      {
        auto si = SchemaItem(field);
        bool invalid = t == capnp::schema::Type::STRUCT && si.children.size() == 0;
        if (!invalid)
          children.push_back(si);
        break;
      }
      default:
        break;
      }
    }
    std::sort(children.begin(), children.end(), [](auto& a, auto &b) {return a.name < b.name;});
  }

  void do_count() {
    if (children.empty()) {
      empty = datasets[dataset_id].time.empty();
      return;
    }
    empty = false;
    for (auto& child : children) {
      child.do_count();
      empty |= child.empty;
    }
  }

  void gui(const std::string& prefix) const
  {
    if (empty)
      return;
    if (children.empty()) {
      if (ImGui::Button(name.c_str())) {
        {
          int xout = nodec++;
          nodes[xout] = new Node {
            .base = xout,
            .type = 1,
            .n = {
              .data = new NodeData {
                .name = prefix + name,
                .x = &(datasets[dataset_id].time),
                .y = &(datasets[dataset_id].value),
              },
            },
          };

          int yout = nodec++;
          nodes[yout] = nodes[xout];

          int xin = nodec++;
          nodes[xin] = new Node {
            .base = xin,
            .type = 0,
            .n = {
              .plot = new NodeNY1XPlot {
                .x = nodes[xout]->n.data->x,
                .ynames = { nodes[xout]->n.data->name, },
                .ys = { nodes[xout]->n.data->y, },
              },
            },
          };
          int yin = nodec++;
          nodes[yin] = nodes[xin];

          links.push_back(std::make_pair(xout, xin));
          links.push_back(std::make_pair(yout, yin));
        }
        if (cur_g != nullptr)
          free(cur_g);
        cur_g = new GraphItem();
        cur_g->name = prefix + name;
        cur_g->dataset = &datasets[dataset_id];
      }
      ImGui::SameLine();
      if (ImGui::Button(("+##" + name).c_str())) {
          int xout = nodec++;
          nodes[xout] = new Node {
            .base = xout,
            .type = 1,
            .n = {
              .data = new NodeData {
                .name = prefix + name,
                .x = &(datasets[dataset_id].time),
                .y = &(datasets[dataset_id].value),
              },
            },
          };

          int yout = nodec++;
          nodes[yout] = nodes[xout];
      }

    } else {
      if (ImGui::TreeNode(name.c_str())) {
        for (const auto& c : children) {
          c.gui(prefix + name + ".");
        }
        ImGui::TreePop();
      }
    }
  }

  void populate_with(capnp::DynamicValue::Reader r, double time) {
    if (children.size() == 0) {
      Dataset& ds = datasets[dataset_id];
      ds.time.push_back(time);
      ds.value.push_back(r.as<double>());
      return;
    }
    auto s = r.as<capnp::DynamicStruct>();
    for (auto& child : children) {
      if (s.has(child.name))
        child.populate_with(s.get(child.name), time);
    }
  }
};

static uint32_t on_mpv_render_update, on_mpv_event;

int main()
{
  //const char *fn = "av/1a393a268e8fae53---2022-07-18--05-10-35--20---qcamera.ts";
  const char *fn = "https://web.kommu.ai/depot/hottub/1a393a268e8fae53/1a393a268e8fae53---2022-07-18--05-10-35";
  const char *rl = "rlog/1a393a268e8fae53---2022-07-18--05-10-35--20---rlog.bz2";

  const char *base = "rlog/1a393a268e8fae53---2022-07-18--05-10-35";

  std::vector<std::string> fields;
  std::vector<SchemaItem> roots;

  capnp::StructSchema schema;
  {
    auto fs = kj::newDiskFilesystem();
    auto parser = new capnp::SchemaParser;
    auto s = parser->parseFromDirectory(fs->getCurrent(), kj::Path::parse("cereal/log.capnp"), nullptr);
    schema = s.getNested("Event").asStruct();

    for (const auto field : schema.getFields()) {
      auto name = field.getProto().getName();
      if (name.endsWith("DEPRECATED") || name == "valid" || name == "logMonoTime")
        continue;
      fields.push_back(name);

      auto si = SchemaItem(field);
      if (si.children.size() > 0)
        roots.push_back(si);
    }
    std::sort(roots.begin(), roots.end(), [](auto& a, auto &b) {return a.name < b.name;});
  }

  {
  uint64_t first_sec = 0;
  int i = 0;
  while (true) {
    std::vector<uint8_t> raw;
    {
      char fname[256];
      sprintf(fname, "%s--%d---rlog.bz2", base, i);
      std::cerr << fname << std::endl;

      FILE *f = fopen(fname, "rb");
      if (!f)
        break;
      auto bzerr = BZ_OK;
      auto bytes = BZ2_bzReadOpen(&bzerr, f, 0, 0, NULL, 0);
      if (bzerr != BZ_OK) {
          std::cerr << "bz error: " << BZ2_bzerror(bytes, &bzerr) << std::endl;
          return -1;
      }
      size_t cur = 0;
      const size_t chunk_sz = 1024 * 1024 * 64;
      while (1) {
        raw.resize(cur + chunk_sz);
        int rsz = BZ2_bzRead(&bzerr, bytes, &raw.data()[cur], chunk_sz);
        switch (bzerr) {
          case BZ_STREAM_END:
            raw.resize(cur + rsz);
            goto finish;
          case BZ_OK:
            cur += chunk_sz;
            break;
          default:
            int smth;
            std::cerr << "bz error: " << BZ2_bzerror(bytes, &smth) << std::endl;
            goto finish;
        }
      }
      finish:
      BZ2_bzReadClose(&bzerr, bytes);
      fclose(f);
    }
    i++;

    auto amsg = kj::ArrayPtr((const capnp::word*)raw.data(), raw.size()/sizeof(capnp::word));
    while (amsg.size() > 0) {
      try {
        auto cmsg = capnp::FlatArrayMessageReader(amsg);
        auto tmsg = new capnp::FlatArrayMessageReader(kj::ArrayPtr(amsg.begin(), cmsg.getEnd()));
        amsg = kj::ArrayPtr(cmsg.getEnd(), amsg.end());

        auto ev = tmsg->getRoot<capnp::DynamicStruct>(schema);
        ev.get("valid");
        if (ev.get("valid").as<bool>() != 1)
          continue;
        if (first_sec == 0 && ev.has("sentinel")) { // there are two sentinel: one head one tail !
          first_sec = ev.get("logMonoTime").as<uint64_t>();
        }

        double time = 0;
        {
          auto tt = ev.get("logMonoTime").as<uint64_t>();
          if (tt < first_sec) // give negative time
            time = ((double) tt - (double) first_sec) / 1e9;
          else
            time = (double) (tt - first_sec) / 1e9;
        }

        // should we do not...?? idk, its slow
        for (auto& root : roots) {
          if (!ev.has(root.name))
            continue;
          auto r = ev.get(root.name);
          root.populate_with(r, time);
        }
      } catch (const kj::Exception& e) {
        std::cerr << e.getDescription().cStr() << std::endl;
        return -1;
      }
    }
  }
  }

  for (auto& root : roots)
    root.do_count();

  ImVec2 fsize = {480, 360};

  auto mpv = mpv_create();
  mpv_initialize(mpv);
  // mpv_request_log_messages(mpv, "debug");
  SDL_SetHint(SDL_HINT_NO_SIGNAL_HANDLERS, "no");

  if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_TIMER | SDL_INIT_GAMECONTROLLER) != 0)
  {
    std::cerr << "Error: " << SDL_GetError() << std::endl;
    return -1;
  }

  const char *glsl_version = "#version 130";
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, 0);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_CORE);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 3);
  SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 0);

  // Create window with graphics context
  SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
  SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 24);
  SDL_GL_SetAttribute(SDL_GL_STENCIL_SIZE, 8);
  SDL_WindowFlags window_flags = (SDL_WindowFlags)(SDL_WINDOW_OPENGL | SDL_WINDOW_RESIZABLE | SDL_WINDOW_ALLOW_HIGHDPI);
  SDL_Window* window = SDL_CreateWindow("neoporteo", SDL_WINDOWPOS_CENTERED, SDL_WINDOWPOS_CENTERED, 1280, 720, window_flags);
  SDL_GLContext gl_context = SDL_GL_CreateContext(window);
  SDL_GL_MakeCurrent(window, gl_context);
  SDL_GL_SetSwapInterval(1); // Enable vsync

  glewInit();

  mpv_set_option_string(mpv, "gpu-hwdec-interop", "no");
  mpv_set_option_string(mpv, "video-timing-offset", "0");
  mpv_set_option_string(mpv, "keep-open", "always");
  mpv_set_option_string(mpv, "pause", "1");

  mpv_opengl_init_params moi = {
    .get_proc_address = [](auto ctx, auto name) {
      return SDL_GL_GetProcAddress(name);
    },
  };
  int one = 1;
  mpv_render_param params[] = {
    {MPV_RENDER_PARAM_API_TYPE, (void *) MPV_RENDER_API_TYPE_OPENGL},
    {MPV_RENDER_PARAM_OPENGL_INIT_PARAMS, &moi},
    {MPV_RENDER_PARAM_ADVANCED_CONTROL, &one},
    {MPV_RENDER_PARAM_INVALID},
  };
  mpv_render_context *mpv_gl;
  mpv_render_context_create(&mpv_gl, mpv, params);
  on_mpv_render_update = SDL_RegisterEvents(1);
  on_mpv_event = SDL_RegisterEvents(1);
  mpv_set_wakeup_callback(mpv, [](auto ctx) {
    SDL_Event ev = {.type = on_mpv_event};
    SDL_PushEvent(&ev);
  }, NULL);
  mpv_render_context_set_update_callback(mpv_gl, [](auto ctx) {
    SDL_Event ev = {.type = on_mpv_render_update};
    SDL_PushEvent(&ev);
  }, NULL);

#define MPV(...) {\
  const char *cmd[] = { __VA_ARGS__, NULL };  \
  mpv_command_async(mpv, 0, cmd);       \
}

  MPV("loadfile", fn);

  double duration = -1;

  // Setup Dear ImGui context
  IMGUI_CHECKVERSION();
  ImGui::CreateContext();
  ImPlot::CreateContext();
  ImNodes::CreateContext();

  auto& io = ImGui::GetIO();
  io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

  ImGui::StyleColorsDark();

  ImGui_ImplSDL2_InitForOpenGL(window, gl_context);
  ImGui_ImplOpenGL3_Init(glsl_version);

  GLuint texture;
  glGenTextures(1, &texture);
  glBindTexture(GL_TEXTURE_2D, texture);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fsize.x, fsize.y, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);

  GLuint fbo;
  glGenFramebuffers(1, &fbo);
  glBindFramebuffer(GL_FRAMEBUFFER, fbo);
  glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

  auto begin = ImGui::GetTime();
  unsigned cur_frame = -1;
  const auto fps = 20;

  bool done = false;

  bool playing = false;
  double movie_delta = 0;
  while (!done) {
    bool redraw = false;

    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      done |= event.type == SDL_QUIT;
      done |= event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window);

      if (event.type == on_mpv_event) {
        mpv_event *mev;
        while ((mev = mpv_wait_event(mpv, 0))->event_id != MPV_EVENT_NONE) {
          switch (mev->event_id) {
            case MPV_EVENT_LOG_MESSAGE:
              std::cout << ((mpv_event_log_message *) mev->data)->text;
              break;
            case MPV_EVENT_FILE_LOADED:
              mpv_get_property_async(mpv, 1337, "duration", MPV_FORMAT_DOUBLE);
              break;
            case MPV_EVENT_GET_PROPERTY_REPLY:
              if (mev->error < 0) {
                std::cerr << "ERR" << std::endl;
                break;
              }
              if (mev->reply_userdata == 1337) {
                duration = *(double *)((mpv_event_property *)mev->data)->data;
                std::cout << "duration: " << duration;
              }
            default:
              break;
          }
        }
      } else if (event.type == on_mpv_render_update) {
        redraw |= (mpv_render_context_update(mpv_gl) & MPV_RENDER_UPDATE_FRAME);
      }
    }
    done |= ImGui::IsKeyReleased(ImGuiKey_Q);

    if (redraw) {
      mpv_opengl_fbo fbo_params = {
        .fbo = (int) fbo,
        .w = (int) fsize.x,
        .h = (int) fsize.y,
      };
      int o = 1;
      mpv_render_param params[] = {
        {MPV_RENDER_PARAM_OPENGL_FBO, &fbo_params},
        // {MPV_RENDER_PARAM_FLIP_Y, &o},
        {MPV_RENDER_PARAM_INVALID},
      };
      mpv_render_context_render(mpv_gl, params);
    }

    playing = (ImGui::IsKeyReleased(ImGuiKey_Space)) ? !playing : playing;
    if (duration > 0) {
      if (playing)
        movie_delta += io.DeltaTime;
      auto need_frame = (unsigned) (movie_delta * fps);
      if (need_frame != cur_frame) {
        if (need_frame - cur_frame == 1) {
          MPV("frame-step");
        } else {
          auto t = std::to_string(need_frame / fps);
          MPV("seek", t.c_str(), "absolute+exact");
          MPV("set", "pause", "yes");
          //MPV("frame-back-step");
        }
        cur_frame = need_frame;
      }
    }

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    //ImGui::ShowMetricsWindow();

    auto vp = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(vp->WorkPos);
    ImGui::SetNextWindowSize(vp->WorkSize);
    ImGui::SetNextWindowViewport(vp->ID);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0.0f, 0.0f));
    auto windowFlags = ImGuiWindowFlags_MenuBar | ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoNavFocus | ImGuiWindowFlags_NoBringToFrontOnFocus | ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse;
    ImGui::Begin("Dockspace", nullptr, windowFlags);
    auto dockId = ImGui::DockSpace(ImGui::GetID("Dockspace"));
    ImGui::PopStyleVar();
    if (ImGui::BeginMenuBar()) {
      if (ImGui::BeginMenu("NEOPORTEO")) {
        if (ImGui::MenuItem("Open Drive")) {
          std::cout << "Open Drive" << std::endl;
        }
        ImGui::Separator();
        if (ImGui::MenuItem("killall neoporteo")) {
          done = true;
        }
        ImGui::EndMenu();
      }
      ImGui::EndMenuBar();
    }

    ImGui::End();

    if (ImGui::Begin("VIDEO::jav.mp4")) {
      ImGui::Image((void *)(intptr_t) texture, fsize);
      ImGui::End();
    }

    if (ImGui::Begin("DATA::msg_name")) {
      for (const auto& si : roots)
        si.gui("");

      ImGui::End();
    }

    if (ImGui::Begin("CTRL::XKeyscore")) {
      ImNodes::BeginNodeEditor();

      for (const auto& [idx, node] : nodes) {
        if (node->base != idx)
          continue;
        const auto& meta = node_dict[node->type];
        ImNodes::BeginNode(idx);
        ImNodes::BeginNodeTitleBar();
        ImGui::TextUnformatted(meta.name.c_str());
        ImNodes::EndNodeTitleBar();
        if (node->type == 1) {
          ImGui::TextUnformatted(node->n.data->name.c_str());
        }
        auto iidx = idx;
        for (const auto& field : meta.ins) {
          ImNodes::BeginInputAttribute(iidx++);
          ImGui::TextUnformatted(field.c_str());
          ImNodes::EndInputAttribute();
        }
        for (const auto& field : meta.outs) {
          ImNodes::BeginOutputAttribute(iidx++);
          ImGui::TextUnformatted(field.c_str());
          ImNodes::EndInputAttribute();
        }
        ImNodes::EndNode();
      }

      int i = 1000;
      for (const auto& [start, end] : links) {
        ImNodes::Link(i++, start, end);
      }
      ImNodes::EndNodeEditor();
      ImGui::End();

      int start_attr, end_attr;
      if (ImNodes::IsLinkCreated(&start_attr, &end_attr)) {
        auto plot = nodes[end_attr];
        if (plot->type != 0)
          continue;
        auto data = nodes[start_attr];
        if (data->type != 1)
          continue;
        std::vector<double> *v ;
        switch (start_attr - data->base) {
        case 0:
          v = data->n.data->x;
          break;
        case 1:
          v = data->n.data->y;
          break;
        }
        auto iidx = end_attr - plot->base;
        switch (iidx) {
        case 0: {
          plot->n.plot->reset = true;
          plot->n.plot->x = v;
          int to_delete = -1, i = 0;
          for (const auto& [start, end] : links) {
            if (end == end_attr)
              to_delete = i;
            i++;
          }
          if (to_delete != -1)
            links.erase(links.begin() + to_delete);
          links.push_back(std::make_pair(start_attr, end_attr));
          break;
        }
        case 1:
          plot->n.plot->reset = true;
          plot->n.plot->ys.push_back(v);
          plot->n.plot->ynames.push_back(data->n.data->name);
          links.push_back(std::make_pair(start_attr, end_attr));
          break;
        }
      }
    }

    int ggid = 1000;

    if (ImGui::Begin("VIZ::GG")) {
      for (const auto& [idx, node] : nodes) {
        if (node->base != idx || node->type != 0)
          continue;
        auto plot = node->n.plot;
        if (plot->reset) {
          ImPlot::SetNextAxesToFit();
          plot->reset = false;
        }
        if (ImPlot::BeginPlot("PLOT")) {
          for (int i = 0; i < plot->ys.size(); i++) {
            ImPlot::PlotLine(plot->ynames[i].c_str(), plot->x->data(), plot->ys[i]->data(), plot->x->size());
            ImPlot::DragLineX(ggid++, &movie_delta, ImVec4(1,1,1,1), 0); //ImPlotDragToolFlags_NoInputs
          }
          ImPlot::EndPlot();
        }
      }
      ImGui::End();
    }

    ImGui::Render();
    glViewport(0, 0, (int)io.DisplaySize.x, (int)io.DisplaySize.y);
    glClearColor(0.45f, 0.55f, 0.60f, 1.00f);
    glClear(GL_COLOR_BUFFER_BIT);
    ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());
    SDL_GL_SwapWindow(window);
  }

  mpv_render_context_free(mpv_gl);
  mpv_detach_destroy(mpv);

  ImNodes::DestroyContext();
  ImPlot::DestroyContext();
  ImGui_ImplOpenGL3_Shutdown();
  ImGui_ImplSDL2_Shutdown();
  ImGui::DestroyContext();

  SDL_GL_DeleteContext(gl_context);
  SDL_DestroyWindow(window);
  SDL_Quit();

  return 0;
}
