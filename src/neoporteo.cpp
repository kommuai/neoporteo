#include <algorithm>
#include <cassert>
#include <cstddef>
#include <cstdio>
#include <iostream>
#include <unordered_map>
#include <string>
#include <utility>
#include <vector>

#include "imgui.h"
#include "imgui_impl_sdl.h"
#include "imgui_impl_opengl3.h"
#include "implot.h"
#include "imnodes.h"

#include <SDL.h>
#include <SDL_opengl.h>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libswscale/swscale.h>
#include <libavformat/avformat.h>
#include <libavutil/imgutils.h>
}

#include <bzlib.h>
#include <capnp/common.h>
#include <capnp/schema-parser.h>
#include <capnp/serialize-packed.h>
#include <capnp/dynamic.h>
#include <kj/array.h>

uint64_t first_sec = 0;

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


int main()
{
  const char *fn = "av/1a393a268e8fae53---2022-07-18--05-10-35--20---qcamera.ts";
  const char *rl = "rlog/1a393a268e8fae53---2022-07-18--05-10-35--20---rlog.bz2";
  AVCodecParameters *params;
  AVFormatContext *fmt_ctx = avformat_alloc_context();
  avformat_open_input(&fmt_ctx, fn, NULL, NULL);
  avformat_find_stream_info(fmt_ctx, NULL);
  params = fmt_ctx->streams[0]->codecpar;

  std::vector<uint8_t *> buffers;

  auto codec = avcodec_find_decoder(params->codec_id);
  auto ctx = avcodec_alloc_context3(codec);
  avcodec_parameters_to_context(ctx, params);
  avcodec_open2(ctx, codec, NULL);
  auto fri = av_frame_alloc();
  auto fr = av_frame_alloc();
  auto pkt = av_packet_alloc();
  auto buf_size = av_image_get_buffer_size(AV_PIX_FMT_RGB24, ctx->width, ctx->height, 1);

  auto resize = sws_getContext(ctx->width, ctx->height, AV_PIX_FMT_YUV420P, ctx->width, ctx->height, AV_PIX_FMT_RGB24, SWS_BICUBIC, NULL, NULL, NULL);
  auto duration = fmt_ctx->duration / AV_TIME_BASE;
  std::cout << "dur: " << duration << std::endl;

  int result;
  ImVec2 fsize;

  fsize.x = ctx->width;
  fsize.y = ctx->height;

  while(av_read_frame(fmt_ctx, pkt) >= 0) {
    uint8_t *buf = (uint8_t *) av_malloc(buf_size);
    avcodec_send_packet(ctx, pkt);
    av_packet_unref(pkt);
    avcodec_receive_frame(ctx, fri);

    av_image_fill_arrays(fr->data, fr->linesize, buf, AV_PIX_FMT_RGB24, ctx->width, ctx->height, 1);
    // sws_scale will fill buffer directly, no need to copy_buffer again!
    sws_scale(resize, fri->data, fri->linesize, 0, fri->height, fr->data, fr->linesize);
    av_frame_unref(fr);

    buffers.push_back(buf);
  }
  auto fps = buffers.size() / duration;

  std::cout << buffers.size() << std::endl;

  av_packet_free(&pkt);
  av_frame_free(&fr);
  avformat_close_input(&fmt_ctx);
  avcodec_free_context(&ctx);

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

  std::unordered_map<std::string, std::vector<capnp::DynamicStruct::Reader>> events;
  {
    std::vector<uint8_t> raw;
    {
      FILE *f = fopen(rl, "rb");
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

        for (const auto field : fields) {
          if (ev.has(field))
            events[field].push_back(ev);
        }
      } catch (const kj::Exception& e) {
        std::cerr << e.getDescription().cStr() << std::endl;
        return -1;
      }
    }
  }

  for (auto& root : roots)
    root.do_count();

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
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
  glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
  glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fsize.x, fsize.y, 0, GL_RGB, GL_UNSIGNED_BYTE, (const GLvoid*) buffers[0]);

  auto begin = ImGui::GetTime();
  unsigned cur_frame = -1;

  bool done = false;
  float drift = 0;

  bool playing = false;
  double movie_delta = 0;
  while (!done) {
    SDL_Event event;
    while (SDL_PollEvent(&event)) {
      ImGui_ImplSDL2_ProcessEvent(&event);
      done |= event.type == SDL_QUIT;
      done |= event.type == SDL_WINDOWEVENT && event.window.event == SDL_WINDOWEVENT_CLOSE && event.window.windowID == SDL_GetWindowID(window);
    }
    done |= ImGui::IsKeyReleased(ImGuiKey_Q);

    playing = (ImGui::IsKeyReleased(ImGuiKey_Space)) ? !playing : playing;

    ImGui_ImplOpenGL3_NewFrame();
    ImGui_ImplSDL2_NewFrame();
    ImGui::NewFrame();

    //ImGui::ShowMetricsWindow();

    if (playing)
      movie_delta += io.DeltaTime;

    auto need_frame = (unsigned) (movie_delta * fps);
    if (need_frame != cur_frame) {
      glBindTexture(GL_TEXTURE_2D, texture);
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, fsize.x, fsize.y, 0, GL_RGB, GL_UNSIGNED_BYTE, (const GLvoid*) buffers[need_frame]);
      cur_frame = need_frame;
    }

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
      ImGui::Text("%.4f", drift);
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
