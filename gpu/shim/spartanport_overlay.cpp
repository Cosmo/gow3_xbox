// SPDX-License-Identifier: GPL-2.0-or-later
#include "spartanport_overlay.h"
#ifdef GOW3_UWP
#include "../../xbox/graphics/overlay.h"
#endif

#include <algorithm>
#include <atomic>
#include <cfloat>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <vector>

#include <SDL3/SDL.h>
#include "spartanport_settings.h"
#include "spartanport_orbs.h"
#include "spartanport_actors.h"
#include "spartanport_graphics.h"
#include "spartanport_perf_stats.h"
#include "../../src/actor_hook.h"

extern SpartanActorSlot spartanport_actor_slots[2];
extern "C" void runtime_restart(void); // src/probe.c: starts run.py again, ends this process
#include "common/elf_info.h"
#include "imgui.h"
#include "imgui_internal.h" // RegisterUserTexture (save pictures)
#include "imgui_impl_vulkan.h"
#include "video_core/renderer_vulkan/vk_instance.h"
#include "video_core/renderer_vulkan/vk_scheduler.h"
#include "video_core/renderer_vulkan/vk_frame_capture.h"
#include "video_core/renderdoc.h"

// DejaVu Sans (Cyrillic), embedded (third_party/fonts, Bitstream Vera license).
#ifdef _WIN32
asm(".section .rdata,\"dr\"\n"
    ".balign 16\n"
    ".global spartanport_font_ttf\n"
    "spartanport_font_ttf:\n"
    ".incbin \"" SPARTAN_FONT_PATH "\"\n"
    ".global spartanport_font_ttf_end\n"
    "spartanport_font_ttf_end:\n"
    ".text\n");
#else
asm(".section .rodata\n"
    ".balign 16\n"
    ".hidden spartanport_font_ttf\n"
    ".global spartanport_font_ttf\n"
    "spartanport_font_ttf:\n"
    ".incbin \"" SPARTAN_FONT_PATH "\"\n"
    ".hidden spartanport_font_ttf_end\n"
    ".global spartanport_font_ttf_end\n"
    "spartanport_font_ttf_end:\n"
    ".previous\n");
#endif
extern "C" const unsigned char spartanport_font_ttf[];
extern "C" const unsigned char spartanport_font_ttf_end[];

namespace SpartanOverlay {

namespace {

std::mutex imgui_mutex; // the ImGui context: window thread (input) and present thread
bool initialized = false;
std::atomic<bool> menu_open{false};
bool r3_down = false, l2_down = false;
std::atomic<bool> loading{false};
std::string loading_label;
std::atomic<u32> loading_done{0}, loading_total{0};
std::atomic<u32> background_done{0}, background_total{0};
// A short message in a corner (saved screenshot), shown until notice_until.
std::mutex notice_mutex;
std::string notice_text;
std::atomic<s64> notice_until{0}; // steady_clock milliseconds
bool dirty = false; // settings changed while open: saved on close
int requested_tab = -1; // switch on the next frame (L1/R1, Q/E, menu opened)
// The game's text dialog (SetTextEntry), guarded by imgui_mutex.
bool text_entry_active = false;
std::string text_entry_prompt, text_entry_text;
// The game's save list (sceSaveDataDialog): a choice made with the arrows/D-pad.
std::atomic<bool> choice_active{false};
std::string choice_title;
std::vector<std::string> choice_items;
int choice_index = 0;
std::atomic<int> choice_result{-1}; // -1 open, -2 cancelled, else the chosen index
// Save pictures: decoded by BeginChoice, made textures on the present thread (null = none).
// Closed lists' textures wait in choice_retired until the backend has destroyed them.
std::vector<SpartanPng::Image> choice_pictures;
std::vector<ImTextureData*> choice_textures, choice_retired;
std::atomic<bool> choice_retiring{false};
float base_scale = 1.0f;

// Present rate for the FPS counter.
std::chrono::steady_clock::time_point last_present{};
float frame_ms_avg = 0.0f;

void SetOpen(bool value) {
    if (menu_open.exchange(value) == value) {
        return;
    }
#ifndef GOW3_UWP // the Xbox has no mouse pointer to draw
    ImGui::GetIO().MouseDrawCursor = value;
#endif
    if (value) {
        requested_tab = SpartanSettings::Get().menu_tab;
    }
    if (!value && dirty) {
        dirty = false;
        SpartanSettings::Save();
    }
}

ImGuiKey KeyFromSdl(SDL_Keycode key) {
    switch (key) {
    case SDLK_TAB: return ImGuiKey_Tab;
    case SDLK_LEFT: return ImGuiKey_LeftArrow;
    case SDLK_RIGHT: return ImGuiKey_RightArrow;
    case SDLK_UP: return ImGuiKey_UpArrow;
    case SDLK_DOWN: return ImGuiKey_DownArrow;
    case SDLK_PAGEUP: return ImGuiKey_PageUp;
    case SDLK_PAGEDOWN: return ImGuiKey_PageDown;
    case SDLK_HOME: return ImGuiKey_Home;
    case SDLK_END: return ImGuiKey_End;
    case SDLK_DELETE: return ImGuiKey_Delete;
    case SDLK_BACKSPACE: return ImGuiKey_Backspace;
    case SDLK_SPACE: return ImGuiKey_Space;
    case SDLK_RETURN: return ImGuiKey_Enter;
    case SDLK_KP_ENTER: return ImGuiKey_KeypadEnter;
    case SDLK_ESCAPE: return ImGuiKey_Escape;
    case SDLK_Q: return ImGuiKey_Q;
    case SDLK_E: return ImGuiKey_E;
    case SDLK_LCTRL: return ImGuiKey_LeftCtrl;
    case SDLK_RCTRL: return ImGuiKey_RightCtrl;
    case SDLK_LSHIFT: return ImGuiKey_LeftShift;
    case SDLK_RSHIFT: return ImGuiKey_RightShift;
    case SDLK_LALT: return ImGuiKey_LeftAlt;
    case SDLK_RALT: return ImGuiKey_RightAlt;
    default: return ImGuiKey_None;
    }
}

ImGuiKey KeyFromGamepad(u8 button) {
    switch (button) {
    case SDL_GAMEPAD_BUTTON_SOUTH: return ImGuiKey_GamepadFaceDown;
    case SDL_GAMEPAD_BUTTON_EAST: return ImGuiKey_GamepadFaceRight;
    case SDL_GAMEPAD_BUTTON_WEST: return ImGuiKey_GamepadFaceLeft;
    case SDL_GAMEPAD_BUTTON_NORTH: return ImGuiKey_GamepadFaceUp;
    case SDL_GAMEPAD_BUTTON_DPAD_UP: return ImGuiKey_GamepadDpadUp;
    case SDL_GAMEPAD_BUTTON_DPAD_DOWN: return ImGuiKey_GamepadDpadDown;
    case SDL_GAMEPAD_BUTTON_DPAD_LEFT: return ImGuiKey_GamepadDpadLeft;
    case SDL_GAMEPAD_BUTTON_DPAD_RIGHT: return ImGuiKey_GamepadDpadRight;
    case SDL_GAMEPAD_BUTTON_LEFT_SHOULDER: return ImGuiKey_GamepadL1;
    case SDL_GAMEPAD_BUTTON_RIGHT_SHOULDER: return ImGuiKey_GamepadR1;
    case SDL_GAMEPAD_BUTTON_START: return ImGuiKey_GamepadStart;
    case SDL_GAMEPAD_BUTTON_BACK: return ImGuiKey_GamepadBack;
    default: return ImGuiKey_None;
    }
}

float PixelDensity(SDL_WindowID id) {
    SDL_Window* window = SDL_GetWindowFromID(id);
    const float density = window ? SDL_GetWindowPixelDensity(window) : 1.0f;
    return density > 0.0f ? density : 1.0f;
}

// Marks the settings dirty when a widget changed them.
template <typename T>
void Store(std::atomic<T>& target, T value, bool changed) {
    if (changed) {
        target = value;
        dirty = true;
    }
}

void Checkbox(const char* label, std::atomic<bool>& value) {
    bool v = value;
    const bool changed = ImGui::Checkbox(label, &v);
    Store(value, v, changed);
}

// A combo over `count` labels storing an index or a value from `values`.
void Choice(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
            int count) {
    int current = 0;
    for (int i = 0; i < count; ++i) {
        if ((values ? values[i] : i) == target.load()) current = i;
    }
    if (ImGui::Combo(label, &current, labels, count)) {
        Store(target, values ? values[current] : current, true);
    }
}

// Controller button names in the help text follow SPARTAN_GLYPHS (run.py sets xbox or nintendo when
// the glyph feature is on); unset is PlayStation. Read once: the environment is fixed for a run.
static const char* PadText(const char* playstation, const char* xbox, const char* nintendo) {
    static const char* style = std::getenv("SPARTAN_GLYPHS");
    if (!style) return playstation;
    return !std::strcmp(style, "xbox") ? xbox : !std::strcmp(style, "nintendo") ? nintendo : playstation;
}

// Settings that only take effect when the game starts: the PC restarts it, the Xbox app cannot restart itself.
#ifdef GOW3_UWP
constexpr const char* RestartNote = "aplicado na próxima inicialização";
#else
constexpr const char* RestartNote = "aplicado ao reiniciar";
#endif
std::string RestartSection(const char* name) {
    return std::string(name) + " (" + RestartNote + ")";
}

// The note for the item under the mouse or the gamepad/keyboard focus, shown in the footer.
const char* footer_help = nullptr;

void Help(const char* text) {
    if (text && (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) || ImGui::IsItemFocused())) {
        footer_help = text;
    }
}

// Two-column rows: the name on the left, the control on the right.
bool BeginRows(const char* id) {
    if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp)) {
        return false;
    }
    ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.52f);
    ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.48f);
    return true;
}

void Row(const char* label) {
    ImGui::TableNextRow();
    ImGui::TableNextColumn();
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(label);
    ImGui::TableNextColumn();
    ImGui::SetNextItemWidth(-FLT_MIN);
    ImGui::PushID(label);
}

void CheckRow(const char* label, std::atomic<bool>& value, const char* help = nullptr) {
    Row(label);
    Checkbox("##value", value);
    Help(help);
    ImGui::PopID();
}

void ChoiceRow(const char* label, std::atomic<int>& target, const char* const* labels, const int* values,
               int count, const char* help = nullptr) {
    Row(label);
    Choice("##value", target, labels, values, count);
    Help(help);
    ImGui::PopID();
}

// A 0.1x to 100x multiplier with a 1x button.
void MultiplierRow(const char* label, std::atomic<float>& value, const char* help) {
    Row(label);
    const ImGuiStyle& style = ImGui::GetStyle();
    ImGui::SetNextItemWidth(-(ImGui::CalcTextSize("1x").x + style.FramePadding.x * 2 + style.ItemSpacing.x));
    float v = value;
    if (ImGui::SliderFloat("##value", &v, 0.1f, 100.0f, "%.2fx",
                           ImGuiSliderFlags_Logarithmic | ImGuiSliderFlags_AlwaysClamp)) {
        value = SpartanOrbs::ClampMultiplier(v);
        dirty = true;
    }
    Help(help);
    ImGui::SameLine();
    if (ImGui::SmallButton("1x")) {
        value = 1.0f;
        dirty = true;
    }
    Help(help);
    ImGui::PopID();
}

void InfoRow(const char* label, const char* text) {
    Row(label);
    ImGui::TextDisabled("%s", text);
    ImGui::PopID();
}

void SliderRow(const char* label, std::atomic<int>& value, int lo, int hi, const char* format, const char* help) {
    Row(label);
    int v = value;
    if (ImGui::SliderInt("##value", &v, lo, hi, format, ImGuiSliderFlags_AlwaysClamp)) {
        Store(value, std::clamp(v, lo, hi), true);
    }
    Help(help);
    ImGui::PopID();
}

const char* UpscalerLabel(int upscaler) {
    switch (upscaler) {
    case SpartanSettings::UpscalerFsr3: return "FSR 3.1";
    case SpartanSettings::UpscalerFsr4: return "FSR 4";
    case SpartanSettings::UpscalerFsr411: return "FSR 4.1.1";
    case SpartanSettings::UpscalerTaa: return "TAA";
    case SpartanSettings::UpscalerDlss: return "DLSS";
    default: return "";
    }
}

const Vulkan::Instance* overlay_instance = nullptr; // for the overlay's measurements

// The performance overlay's text with the current settings (also the menu's preview).
std::string OverlayText() {
    namespace P = SpartanPerfStats;
    const auto& s = SpartanSettings::Get();
    const unsigned items = s.overlay_items;
    if ((items & (P::Gpu | P::Cpu | P::Ram | P::Vram)) && overlay_instance) {
        P::Start(*overlay_instance);
    }
    P::Sample sample = P::Latest();
    sample.fps = frame_ms_avg > 0.0f ? 1000.0f / frame_ms_avg : 0.0f;
    sample.frame_ms = frame_ms_avg;
    sample.upscaler = UpscalerLabel(s.upscaler);
    return P::Format(sample, items, s.overlay_layout == 0);
}

constexpr const char* Unsupported = "Indisponível: a versão do jogo ou a assinatura do código não confere.";

void GameTab() {
    auto& s = SpartanSettings::Get();
    ImGui::SeparatorText("Trapaças");
    if (BeginRows("##cheats")) {
        static const char* const names[] = {"Vida infinita", "Magia infinita", "Medidor de itens infinito",
                                            "Fúria de Esparta infinita", "Orbes vermelhos no máximo",
                                            "Desbloquear Muito Difícil", "Saltos duplos infinitos"};
        static const char* const helps[] = {
            "A vida fica cheia. Só Kratos é protegido. Desativar não desfaz recargas anteriores.",
            "A magia fica cheia.", "O medidor de itens (arco, cabeça de Hélio, botas) fica cheio.",
            "A Fúria de Esparta fica cheia.", "Os orbes vermelhos ficam no máximo.",
            "Ative antes de abrir Novo Jogo. Não muda a dificuldade de um save existente.",
            "Repete saltos no ar sem tocar o chão. Desative para o movimento normal."};
        for (unsigned cheat = 0; cheat < SPARTAN_CHEAT_COUNT; ++cheat) {
            const bool available = (s.cheats_supported.load() & (1u << cheat)) != 0;
            ImGui::BeginDisabled(!available);
            CheckRow(names[cheat], s.cheats[cheat], available ? helps[cheat] : Unsupported);
            ImGui::EndDisabled();
        }
        ImGui::BeginDisabled(!s.qte_supported.load());
        CheckRow("QTE automático", s.auto_qte,
                 s.qte_supported.load() ? "Os comandos de quick-time se resolvem sozinhos: toques únicos, "
                                          "toques repetidos, botões segurados e giros do analógico."
                                        : Unsupported);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Controle");
    if (BeginRows("##controller")) {
        SliderRow("Vibração", s.vibration, 0, 100, "%d%%",
                  "Intensidade da vibração do controle. Em 0%, desliga.");
#ifndef GOW3_UWP // the Xbox has no mouse
        SliderRow("Distância do comando do mouse", s.mouse_prompt_distance, 16, 200, "%d px",
                  "Com os comandos de analógico no modo Mouse adaptativo: quanto o mouse se move para cada passo de uma rotação.");
        static const char* const aim_styles[] = {"Relativo", "Absoluto"};
        ChoiceRow("Estilo do analógico direito", s.mouse_aim_style, aim_styles, nullptr, 2,
                  "Mira do L2 com o mouse (Cabeça de Hélio, Arco de Apolo). Relativo: o analógico acompanha o "
                  "movimento e volta ao neutro quando o mouse para. Absoluto: o analógico aponta de "
                  "onde o mouse estava quando o L2 foi pressionado até onde ele está.");
        SliderRow("Sensibilidade do analógico direito", s.mouse_aim_sensitivity, 10, 200, "%d%%",
                  "Mira do L2 com o mouse: menor exige mais movimento do mouse para o mesmo movimento do analógico, maior exige menos.");
        CheckRow("Manter o mouse na janela", s.mouse_lock,
                 "Enquanto você joga, o mouse fica na janela do jogo; este menu e outras janelas o recuperam.");
#endif
        ImGui::EndTable();
    }

    ImGui::SeparatorText("Multiplicadores");
    if (ImGui::Button("Redefinir tudo para 1x")) {
        for (auto* m : {&s.red_orb_multiplier, &s.green_orb_multiplier, &s.blue_orb_multiplier,
                        &s.gold_orb_multiplier, &s.damage_dealt, &s.damage_taken}) {
            *m = 1.0f;
        }
        dirty = true;
    }
    Help("Todos os multiplicadores voltam a 1x (os valores normais do jogo).");
    if (BeginRows("##multipliers")) {
        const bool max_orbs = (s.cheats_supported.load() & (1u << 4)) && s.cheats[4].load();
        ImGui::BeginDisabled(!s.red_orbs_supported.load() || max_orbs);
        MultiplierRow("Orbes vermelhos", s.red_orb_multiplier,
                      !s.red_orbs_supported.load() ? Unsupported
                      : max_orbs ? "Desative Orbes vermelhos no máximo para usar o multiplicador."
                                 : "Orbes vermelhos ganhos. Os orbes que você tem e os preços não mudam.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.orb_pickup_supported.load());
        const char* pickup = s.orb_pickup_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Orbes verdes (vida)", s.green_orb_multiplier,
                      pickup ? pickup : "Vida de cada orbe verde. A barra fica dentro do máximo.");
        MultiplierRow("Orbes azuis (magia)", s.blue_orb_multiplier,
                      pickup ? pickup : "Magia de cada orbe azul. A barra fica dentro do máximo.");
        MultiplierRow("Orbes dourados (Fúria de Esparta)", s.gold_orb_multiplier,
                      pickup ? pickup : "Fúria de Esparta de cada orbe dourado. A barra fica dentro do máximo.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.damage_supported.load());
        const char* damage = s.damage_supported.load() ? nullptr : Unsupported;
        MultiplierRow("Dano causado", s.damage_dealt, damage ? damage : "Dano que Kratos causa aos inimigos.");
        MultiplierRow("Dano recebido", s.damage_taken, damage ? damage : "Dano que Kratos recebe.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }

    ImGui::SeparatorText("HUD");
    if (BeginRows("##hud")) {
        ImGui::BeginDisabled(!s.actor_watch_supported.load());
        CheckRow("Barra de vida do inimigo", s.enemy_health_bar,
                 s.actor_watch_supported.load() ? "Uma barra fina no topo para o último inimigo atingido." : Unsupported);
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
}

void ImageTab() {
    namespace G = SpartanGraphics;
    auto& s = SpartanSettings::Get();
#ifndef GOW3_UWP // the Xbox shows the game full screen in step with the TV, without sharpening
    ImGui::SeparatorText("Tela");
    if (BeginRows("##display")) {
        ChoiceRow("Modo de exibição", s.display_mode, G::DisplayModeNames.data(), nullptr, G::DisplayModeCount,
                  "Janela, janela sem bordas ou tela cheia exclusiva.");
        CheckRow("VSync", s.vsync, "Espera o monitor: sem rasgos na imagem, um pouco mais de atraso nos comandos.");
        ImGui::EndTable();
    }
    const int iw = s.image_width, ih = s.image_height, ww = s.window_width, wh = s.window_height;
    const bool downscaling = iw > ww || ih > wh;
    const bool rcas_blocked = s.rcas && !s.rcas_applied;
    ImGui::SeparatorText("Nitidez");
    if (BeginRows("##scaling")) {
        ImGui::BeginDisabled(rcas_blocked);
        CheckRow("Nitidez (RCAS)", s.rcas,
                 rcas_blocked && downscaling ? "Sem efeito: a imagem do jogo é maior que a janela."
                 : rcas_blocked              ? "Sem efeito: a imagem do jogo é menor que a janela."
                                             : "Deixa a imagem final mais nítida.");
        ImGui::EndDisabled();
        ImGui::BeginDisabled(!s.rcas || rcas_blocked);
        SliderRow("Intensidade da nitidez", s.rcas_strength, 0, 100, "%d%%", "Quão forte é a nitidez.");
        ImGui::EndDisabled();
        ImGui::EndTable();
    }
#endif
    ImGui::SeparatorText(RestartSection("Texturas").c_str());
    if (BeginRows("##textures")) {
        static const char* const anisotropy_labels[] = {"Padrão do jogo", "2x", "4x", "8x", "16x"};
        ChoiceRow("Filtragem anisotrópica", s.anisotropy, anisotropy_labels, G::Anisotropies.data(),
                  int(G::Anisotropies.size()),
                  "Chão e paredes vistos de lado ficam mais nítidos. Custo baixo na maioria das GPUs.");
        ImGui::EndTable();
    }
#ifndef GOW3_UWP
    ImGui::TextDisabled("F12 salva uma captura de tela na pasta user\\screenshots.");
#endif
    ImGui::SeparatorText("Informações");
    if (BeginRows("##image_info")) {
        char text[96];
        std::snprintf(text, sizeof(text), "%d x %d", int(s.image_width), int(s.image_height));
        InfoRow("Imagem do jogo", text);
#ifndef GOW3_UWP
        std::snprintf(text, sizeof(text), "%d x %d", int(s.window_width), int(s.window_height));
        InfoRow("Janela", text);
        InfoRow("Nitidez", s.rcas_applied ? "aplicada" : "não aplicada");
#endif
        ImGui::EndTable();
    }
}

void PerformanceTab() {
    namespace G = SpartanGraphics;
    auto& s = SpartanSettings::Get();
    ImGui::SeparatorText("Em tempo real");
    if (BeginRows("##live")) {
#ifdef GOW3_UWP
        // The Xbox runs the engine at 60 FPS: higher limits mean no limit.
        static const char* const fps_labels[] = {"30", "60 (sem limite)"};
        static constexpr int fps_values[] = {30, 0};
        ChoiceRow("Limite de FPS", s.fps_limit, fps_labels, fps_values, 2, "Limita a taxa de quadros.");
#else
        static const char* const fps_labels[] = {"30", "60", "100", "120", "144", "180", "240", "Ilimitado"};
        ChoiceRow("Limite de FPS", s.fps_limit, fps_labels, G::FpsLimits.data(), int(G::FpsLimits.size()),
                  "Limita a taxa de quadros. A taxa do motor (abaixo) é o máximo que ela pode atingir.");
        static const char* const engine_labels[] = {"60 (original)", "100", "120", "144", "180", "240"};
        static_assert(std::size(engine_labels) == G::EngineFps.size());
        ChoiceRow("Taxa de quadros do motor", s.engine_fps, engine_labels, G::EngineFps.data(), int(G::EngineFps.size()),
                  "A maior taxa de quadros em que o próprio jogo roda. 60 para trechos que quebram acima disso (uma beirada "
                  "que Kratos não consegue cruzar, um comando de apertar botões repetidamente). R3 + L1 + R1 ou F9 percorrem a lista.");
        const int effective = G::EffectiveFps(s.fps_limit, SpartanSettings::EngineFpsNow());
        if (s.fps_limit == 0 || effective < s.fps_limit) {
            char text[48];
            std::snprintf(text, sizeof(text), "%d FPS (engine frame rate)", effective);
            InfoRow("Limite efetivo", text);
        }
        if (s.applied_engine_fps <= 60 && s.engine_fps > 60) {
            InfoRow("Taxa de quadros do motor", "60 FPS: o patch de taxa de quadros não está disponível para esta versão do jogo");
        }
#endif
        static const char* const queued_labels[] = {"1 (menor latência)", "2", "Ilimitado"};
        ChoiceRow("Quadros na fila", s.frames_queued, queued_labels, G::FramesQueued.data(),
                  int(G::FramesQueued.size()), "Mais quadros na fila: FPS mais estável, resposta mais lenta aos comandos.");
        CheckRow("Compilar novos shaders em segundo plano", s.async_shaders,
                 "Menos engasgos em áreas novas; um objeto ou efeito pode aparecer um instante depois.");
        ImGui::EndTable();
    }
#ifdef GOW3_UWP
    ImGui::SeparatorText("Aplicado na próxima inicialização");
#else
    ImGui::SeparatorText("Aplicado ao reiniciar");
#endif
    if (BeginRows("##restart")) {
#ifdef GOW3_UWP
        // The resolutions the Xbox app prepares patches for; it reads the choice when it starts.
        static const char* const resolution_labels[] = {"720p · 1280 × 720", "900p · 1600 × 900",
                                                        "1080p · 1920 × 1080 (nativa)", "4K · 3840 × 2160"};
        static const int resolution_values[] = {G::ParseResolution("720p"), G::ParseResolution("900p"),
                                                G::ParseResolution("native"), G::ParseResolution("4K")};
        ChoiceRow("Resolução de renderização", s.render_resolution, resolution_labels, resolution_values, 4,
                  "A resolução em que o jogo desenha. Maior é mais nítido e mais lento; 4K pode não caber na "
                  "memória do Xbox.");
#else
        // The built-in sizes, then those of the enabled mods' patch files (ultrawide, for one).
        // The same labels as the launcher (graphics_options.resolution_label): name, exact size, aspect ratio.
        static const char* const builtin_labels[] = {
            "Native · 1920 × 1080 · 16:9", "480p · 854 × 480 · 16:9",
            "720p · 1280 × 720 · 16:9", "900p · 1600 × 900 · 16:9",
            "1440p · 2560 × 1440 · 16:9",
            "1800p · 3200 × 1800 · 16:9", "4K · 3840 × 2160 · 16:9",
            "2560 × 1080 · 21:9 (beta)", "3440 × 1440 · 21:9 (beta)",
            "3840 × 1600 · 21:9 (beta)", "5120 × 1440 · 32:9 (beta)",
            "1920 × 1200 · 16:10 (beta)", "2560 × 1600 · 16:10 (beta)",
            "1280 × 800 · 16:10", "1440 × 900 · 16:10",
            "1360 × 768 · 16:9",
            "1280 × 960 · 4:3", "1440 × 1080 · 4:3 (beta)", "1920 × 1440 · 4:3 (beta)",
            "2048 × 1536 · 4:3 (beta)", "2560 × 1920 · 4:3 (beta)", "5120 × 2160 · 21:9 (beta)",
            "8K · 7680 × 4320 · 16:9 (beta)"};
        static_assert(std::size(builtin_labels) == G::Resolutions.size());
        const std::vector<std::string> resolution_names = G::ResolutionNames();
        std::vector<const char*> resolution_labels;
        for (std::size_t i = 0; i < resolution_names.size(); ++i) {
            resolution_labels.push_back(i < G::Resolutions.size() ? builtin_labels[i] : resolution_names[i].c_str());
        }
        ChoiceRow("Resolução de renderização", s.render_resolution, resolution_labels.data(), nullptr,
                  int(resolution_labels.size()), "A resolução em que o jogo desenha. Maior é mais nítido e mais lento.");
#endif
        CheckRow("O jogo lê dados da GPU sem esperar", s.stale_readback,
                 "Grande ganho de FPS. Desative se a iluminação ou os objetos parecerem errados.");
        CheckRow("Leituras da GPU adiadas", s.deferred_readback,
                 "Menos esperas pela GPU. Desative se as texturas ou sombras parecerem erradas.");
        ImGui::EndTable();
    }
}

void OverlayTab() {
    namespace P = SpartanPerfStats;
    auto& s = SpartanSettings::Get();
    if (BeginRows("##overlay_on")) {
        CheckRow("Mostrar overlay", s.show_fps, "O overlay de desempenho em um canto da tela.");
        ImGui::EndTable();
    }
    ImGui::BeginDisabled(!s.show_fps);
    ImGui::SeparatorText("O que mostrar");
    if (ImGui::BeginTable("##overlay_items", 2, ImGuiTableFlags_SizingStretchProp)) {
        ImGui::TableNextColumn();
        static const char* const names[] = {"FPS", "Tempo de quadro", "Upscaler", "GPU (nome e uso)", "CPU (jogo)",
                                            "RAM (jogo / total)", "VRAM (usada / disponível)"};
        for (unsigned item = 0; item < 7; ++item) {
#ifdef GOW3_UWP
            if (item == 2 || item == 3) {
                continue; // the Xbox build has no upscaler and cannot read the GPU's use
            }
#endif
            const unsigned bit = 1u << item;
            bool on = (s.overlay_items & bit) != 0;
            if (ImGui::Checkbox(names[item], &on)) {
                s.overlay_items = on ? (s.overlay_items | bit) : (s.overlay_items & ~bit);
                dirty = true;
            }
        }
        ImGui::TableNextColumn();
        ImGui::TextDisabled("Pré-visualização");
        ImGui::BeginChild("##preview", ImVec2(0, 0), ImGuiChildFlags_Borders | ImGuiChildFlags_AutoResizeY);
        ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
        const std::string text = OverlayText();
        ImGui::TextUnformatted(text.empty() ? "(nada selecionado)" : text.c_str());
        ImGui::SetWindowFontScale(1.0f);
        ImGui::EndChild();
        ImGui::EndTable();
    }
    ImGui::SeparatorText("Aparência");
    if (BeginRows("##appearance")) {
        SliderRow("Tamanho da fonte", s.overlay_scale, 50, 300, "%d%%", "Tamanho do texto do overlay.");
        static const char* const corners[] = {"Superior esquerdo", "Superior direito", "Inferior esquerdo", "Inferior direito"};
        ChoiceRow("Canto", s.overlay_corner, corners, nullptr, 4, "Onde o overlay fica.");
        SliderRow("Opacidade do fundo", s.overlay_opacity, 0, 100, "%d%%", "0% é sem fundo.");
        static const char* const layouts[] = {"Uma linha", "Um item por linha"};
        ChoiceRow("Disposição", s.overlay_layout, layouts, nullptr, 2, "Itens lado a lado ou empilhados.");
        ImGui::EndTable();
    }
    ImGui::EndDisabled();
    ImGui::SeparatorText("Sobre");
    if (BeginRows("##about")) {
        const auto& elf = Common::ElfInfo::Instance();
        char text[160];
        std::snprintf(text, sizeof(text), "%.*s v%.*s", int(elf.GameSerial().size()), elf.GameSerial().data(),
                      int(elf.AppVer().size()), elf.AppVer().data());
        InfoRow("Jogo", text);
        const u32 total = background_total;
        if (total) {
            std::snprintf(text, sizeof(text), "carregando %u%%", u32(u64(background_done) * 100 / total));
        } else {
            std::snprintf(text, sizeof(text), "pronto");
        }
        InfoRow("Cache de shaders", text);
        std::string hooks;
        const auto add = [&](bool on, const char* name) {
            if (on) {
                hooks += hooks.empty() ? name : std::string(", ") + name;
            }
        };
        add(s.cheats_supported.load() != 0, "cheats");
        add(s.red_orbs_supported.load(), "orbes");
        add(s.orb_pickup_supported.load(), "coleta");
        add(s.damage_supported.load(), "dano");
        add(s.actor_watch_supported.load(), "inimigos");
        add(s.qte_supported.load(), "QTE");
        InfoRow("Extensões do jogo", hooks.empty() ? "nenhum" : hooks.c_str());
        ImGui::EndTable();
    }
}

// Startup options changed since this launch (applied by "Apply and restart").
int RestartChanges() {
    const auto& s = SpartanSettings::Get();
    return (s.render_resolution != s.startup_render_resolution) +
           (s.deferred_readback != s.startup_deferred_readback) + (s.stale_readback != s.startup_stale_readback) +
           (s.anisotropy != s.startup_anisotropy);
}

void Menu() {
    auto& s = SpartanSettings::Get();
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Appearing, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(std::min(760.0f * base_scale, viewport->WorkSize.x * 0.95f),
                                    std::min(680.0f * base_scale, viewport->WorkSize.y * 0.9f)),
                             ImGuiCond_Appearing);
    bool keep_open = true;
    // spartanport: the game's own title (param.sfo), not a fixed one.
    static const std::string heading = [] {
        const std::string_view title = Common::ElfInfo::Instance().Title();
        return std::string(title.empty() ? "SpartanPort" : title) + PadText("  (Insert / R3+L2)", "  (Insert / RS+LT)", "  (Insert / RS+ZL)");
    }();
    if (!ImGui::Begin(heading.c_str(), &keep_open, ImGuiWindowFlags_NoCollapse)) {
        ImGui::End();
        return;
    }
    footer_help = nullptr;
    constexpr int TabCount = 4;
    const ImGuiIO& io = ImGui::GetIO();
    if (!io.WantTextInput) {
        const int step = ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false) || ImGui::IsKeyPressed(ImGuiKey_E, false) ? 1
                         : ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false) || ImGui::IsKeyPressed(ImGuiKey_Q, false)
                             ? -1
                             : 0;
        if (step) {
            requested_tab = (s.menu_tab + step + TabCount) % TabCount;
        }
    }
    const float footer = ImGui::GetFrameHeightWithSpacing() * 2.0f + ImGui::GetTextLineHeightWithSpacing() * 2.0f;
    if (ImGui::BeginTabBar("##tabs")) {
        static const char* const tabs[TabCount] = {"Jogo", "Imagem", "Desempenho", "Overlay"};
        for (int tab = 0; tab < TabCount; ++tab) {
            const ImGuiTabItemFlags flags = requested_tab == tab ? ImGuiTabItemFlags_SetSelected : 0;
            if (!ImGui::BeginTabItem(tabs[tab], nullptr, flags)) {
                continue;
            }
            if (s.menu_tab != tab && requested_tab < 0) {
                s.menu_tab = tab;
                dirty = true;
            }
            if (ImGui::BeginChild("##body", ImVec2(0, -footer))) {
                switch (tab) {
                case 0: GameTab(); break;
                case 1: ImageTab(); break;
                case 2: PerformanceTab(); break;
                default: OverlayTab(); break;
                }
            }
            ImGui::EndChild();
            ImGui::EndTabItem();
        }
        if (requested_tab >= 0) {
            s.menu_tab = requested_tab;
            dirty = true;
            requested_tab = -1;
        }
        ImGui::EndTabBar();
    }

    ImGui::Separator();
    ImGui::PushTextWrapPos(0.0f);
#ifdef GOW3_UWP // the app keeps its settings in its own storage
    ImGui::TextDisabled("%s", footer_help ? footer_help : "LB / RB: trocar de aba. RS + LT: fechar.");
#else
    ImGui::TextDisabled("%s", footer_help ? footer_help : PadText("L1 / R1 ou Q / E: trocar de aba. As configurações são salvas em spartanport.ini.", "LB / RB ou Q / E: trocar de aba. As configurações são salvas em spartanport.ini.", "L / R ou Q / E: trocar de aba. As configurações são salvas em spartanport.ini."));
#endif
    ImGui::PopTextWrapPos();
    if (const int changes = RestartChanges()) {
#ifdef GOW3_UWP
        // The app cannot restart itself; it reads these settings the next time it starts.
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%d alteraç%s %s em vigor na próxima vez que você iniciar o app.", changes, changes == 1 ? "ão" : "ões", changes == 1 ? "entra" : "entram");
    } else {
        ImGui::NewLine();
    }
#else
        ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%d change%s need%s a restart (unsaved progress is lost).",
                           changes, changes == 1 ? "" : "s", changes == 1 ? "s" : "");
        ImGui::SameLine();
        ImGui::BeginDisabled(!std::getenv("SPARTAN_RESTART_COMMAND"));
        if (ImGui::Button("Aplicar e reiniciar")) {
            // The new launch confirms the options once the game shows; run.py restores the
            // previous ones if it never does (restart_unconfirmed).
            s.restart_unconfirmed = true;
            SpartanSettings::Save();
            runtime_restart();
        }
        ImGui::EndDisabled();
    } else {
        ImGui::NewLine();
    }
#endif
    const float close_width = ImGui::CalcTextSize("Fechar").x + ImGui::GetStyle().FramePadding.x * 2;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + ImGui::GetContentRegionAvail().x - close_width);
    if (ImGui::Button("Fechar")) {
        keep_open = false;
    }
    ImGui::End();
    if (!keep_open) {
        SetOpen(false);
    }
}

// Enemy health bar from the damage hook's records (spartanport_actors.cpp).
// Only the copied values are read, never the game's memory: an actor may be gone by now.
void HealthDisplay() {
    struct Seen {
        uint32_t seq = 0;
        std::chrono::steady_clock::time_point at{};
    };
    static Seen seen[2];
    const auto& s = SpartanSettings::Get();
    if (!s.actor_watch_supported.load()) {
        return;
    }
    const auto now = std::chrono::steady_clock::now();
    SpartanActorSlot slot[2];
    for (int i = 0; i < 2; ++i) {
        std::memcpy(&slot[i], const_cast<const SpartanActorSlot*>(&spartanport_actor_slots[i]), sizeof(slot[i]));
        if (slot[i].seq != seen[i].seq) {
            seen[i] = {slot[i].seq, now};
        }
    }
    const auto recent = [&](int i, int seconds) {
        return seen[i].seq != 0 && now - seen[i].at < std::chrono::seconds(seconds);
    };
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImDrawList* draw = ImGui::GetForegroundDrawList();
    const float scale = base_scale;
    if (s.enemy_health_bar && recent(1, 4) && SpartanActors::Valid(slot[1].health, slot[1].max_health)) {
        // Minimal: a thin green bar centered on the screen's top edge; it shrinks toward the center
        // so it stays symmetric, with the health in numbers below.
        const ImVec2 display = ImGui::GetIO().DisplaySize;
        const float width = 260.0f * scale, height = 5.0f * scale, center = display.x * 0.5f;
        const float top = 36.0f * scale;
        const float fraction = SpartanActors::Fraction(slot[1].health, slot[1].max_health);
        draw->AddRectFilled(ImVec2(center - width * 0.5f - scale, top - scale),
                            ImVec2(center + width * 0.5f + scale, top + height + scale),
                            IM_COL32(0, 0, 0, 140), 2 * scale);
        const float half = width * 0.5f * fraction;
        draw->AddRectFilled(ImVec2(center - half, top), ImVec2(center + half, top + height),
                            IM_COL32(70, 190, 80, 220), 2 * scale);
        char text[48];
        std::snprintf(text, sizeof(text), "%.0f / %.0f", std::ceil(slot[1].health), slot[1].max_health);
        const float font = ImGui::GetFontSize() * 0.9f;
        const ImVec2 size = ImGui::GetFont()->CalcTextSizeA(font, FLT_MAX, 0.0f, text);
        const ImVec2 at{center - size.x * 0.5f, top + height + 4.0f * scale};
        draw->AddText(ImGui::GetFont(), font, ImVec2(at.x + scale, at.y + scale), IM_COL32(0, 0, 0, 160), text);
        draw->AddText(ImGui::GetFont(), font, at, IM_COL32(225, 235, 220, 220), text);
    }
}

// The game's text dialog: what is typed, and how to finish (keyboard or controller).
void TextEntryBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.42f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.32f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, FLT_MAX));
    ImGui::SetNextWindowBgAlpha(0.92f);
    ImGui::Begin("##text_entry", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", text_entry_prompt.c_str());
    ImGui::Separator();
    ImGui::SetWindowFontScale(1.4f);
    ImGui::Text("%s_", text_entry_text.c_str());
    ImGui::SetWindowFontScale(1.0f);
    ImGui::Separator();
    ImGui::TextDisabled("Teclado: digite, Backspace para apagar, Enter = OK, Esc = cancelar");
    ImGui::TextDisabled("%s", PadText("Controle: Cross (A) = OK, Circle (B) = cancelar", "Controle: A = OK, B = cancelar", "Controle: B = OK, A = cancelar"));
    ImGui::End();
}

void ChoiceBox() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSizeConstraints(ImVec2(viewport->WorkSize.x * 0.36f, 0.0f),
                                        ImVec2(viewport->WorkSize.x * 0.8f, viewport->WorkSize.y * 0.8f));
    ImGui::SetNextWindowBgAlpha(0.94f);
    ImGui::Begin("##choice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "%s", choice_title.c_str());
    ImGui::Separator();
    // Save pictures are 228x128; rows without one keep the text aligned when others have one.
    const bool pictures = std::any_of(choice_textures.begin(), choice_textures.end(),
                                      [](ImTextureData* tex) { return tex != nullptr; });
    const ImVec2 picture_size(228.0f * base_scale, 128.0f * base_scale);
    for (int i = 0; i < int(choice_items.size()); ++i) {
        const bool selected = i == choice_index;
        ImGui::BeginGroup();
        ImTextureData* tex = i < int(choice_textures.size()) ? choice_textures[i] : nullptr;
        if (tex) {
            ImGui::Image(tex->GetTexRef(), picture_size);
        } else if (pictures) {
            ImGui::Dummy(picture_size);
        }
        if (pictures) {
            ImGui::SameLine();
        }
        if (selected) {
            ImGui::TextColored(ImVec4(0.0f, 1.0f, 0.0f, 1.0f), "> %s", choice_items[i].c_str());
        } else {
            ImGui::Text("  %s", choice_items[i].c_str());
        }
        ImGui::EndGroup();
        if (selected) {
            if (pictures) {
                const ImVec2 min = ImGui::GetItemRectMin(), max = ImGui::GetItemRectMax();
                const float pad = 3.0f * base_scale;
                ImGui::GetWindowDrawList()->AddRect(ImVec2(min.x - pad, min.y - pad),
                                                    ImVec2(max.x + pad, max.y + pad),
                                                    IM_COL32(255, 230, 140, 255), 0.0f, 0,
                                                    2.0f * base_scale);
            }
            ImGui::SetScrollHereY();
        }
    }
    ImGui::Separator();
    ImGui::TextDisabled("%s", PadText("Cima/Baixo ou D-pad: escolher   Enter ou Cross (A): OK   Esc ou Circle (B): cancelar", "Cima/Baixo ou D-pad: escolher   Enter ou A: OK   Esc ou B: cancelar", "Cima/Baixo ou D-pad: escolher   Enter ou B: OK   Esc ou A: cancelar"));
    ImGui::End();
}

void LoadingScreen() {
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x * 0.5f,
                                   viewport->WorkPos.y + viewport->WorkSize.y * 0.5f),
                            ImGuiCond_Always, ImVec2(0.5f, 0.5f));
    ImGui::SetNextWindowSize(ImVec2(viewport->WorkSize.x * 0.4f, 0.0f), ImGuiCond_Always);
    ImGui::SetNextWindowBgAlpha(0.0f);
    ImGui::Begin("##loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoInputs |
                     ImGuiWindowFlags_NoNav | ImGuiWindowFlags_NoFocusOnAppearing);
    const u32 done = loading_done, total = loading_total;
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextUnformatted(loading_label.empty()
                              ? (total ? "Carregando cache de shaders..." : "Preparando gráficos...")
                              : loading_label.c_str());
    ImGui::SetWindowFontScale(1.0f);
    if (total) {
        char count[64];
        std::snprintf(count, sizeof(count), "%u / %u (%u%%)", done, total,
                      u32(u64(done) * 100 / total));
        ImGui::ProgressBar(float(done) / float(total), ImVec2(-1.0f, 0.0f), count);
    }
    ImGui::TextDisabled(loading_label.empty() ? "A primeira inicialização após uma atualização demora mais." :
                       "Os demais shaders serão preparados durante o jogo.");
    ImGui::End();
}

void FpsCounter() {
    const auto& s = SpartanSettings::Get();
    const std::string text = OverlayText();
    if (text.empty()) {
        return;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    const int corner = s.overlay_corner;
    const bool right = corner == 1 || corner == 3, bottom = corner >= 2;
    ImGui::SetNextWindowPos(ImVec2(right ? viewport->WorkPos.x + viewport->WorkSize.x - pad : viewport->WorkPos.x + pad,
                                   bottom ? viewport->WorkPos.y + viewport->WorkSize.y - pad : viewport->WorkPos.y + pad),
                            ImGuiCond_Always, ImVec2(right ? 1.0f : 0.0f, bottom ? 1.0f : 0.0f));
    ImGui::SetNextWindowBgAlpha(s.overlay_opacity / 100.0f);
    ImGui::Begin("##fps", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::SetWindowFontScale(s.overlay_scale / 100.0f);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

void BackgroundLoading() {
    const u32 done = background_done, total = std::max(background_total.load(), 1u);
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(0.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.35f);
    ImGui::Begin("##background_loading", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextDisabled("Carregando cache de shaders %u%%", u32(u64(done) * 100 / total));
    ImGui::End();
}

s64 NowMs() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::steady_clock::now().time_since_epoch())
        .count();
}

bool NoticeActive() {
    return notice_until.load(std::memory_order_relaxed) > NowMs();
}

void Notice() {
    std::string text;
    {
        std::scoped_lock lock{notice_mutex};
        text = notice_text;
    }
    const ImGuiViewport* viewport = ImGui::GetMainViewport();
    const float pad = 12.0f * base_scale;
    ImGui::SetNextWindowPos(ImVec2(viewport->WorkPos.x + viewport->WorkSize.x - pad,
                                   viewport->WorkPos.y + viewport->WorkSize.y - pad),
                            ImGuiCond_Always, ImVec2(1.0f, 1.0f));
    ImGui::SetNextWindowBgAlpha(0.6f);
    ImGui::Begin("##notice", nullptr,
                 ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_AlwaysAutoResize |
                     ImGuiWindowFlags_NoInputs | ImGuiWindowFlags_NoNav |
                     ImGuiWindowFlags_NoFocusOnAppearing);
    ImGui::TextUnformatted(text.c_str());
    ImGui::End();
}

} // namespace

void ShowNotice(const std::string& text, int milliseconds) {
    {
        std::scoped_lock lock{notice_mutex};
        notice_text = text;
    }
    notice_until.store(NowMs() + milliseconds, std::memory_order_relaxed);
}

void CycleEngineFps() {
    const int fps = SpartanSettings::CycleEngineFps();
    ShowNotice(fps ? "Taxa de quadros do motor: " + std::to_string(fps) + " FPS"
                   : std::string("Taxa de quadros do motor: somente 60 FPS (sem patch de taxa de quadros para esta versão do jogo)"));
}

void Init(const Vulkan::Instance& instance, vk::Format format, u32 image_count) {
    std::scoped_lock lock{imgui_mutex};
    if (initialized) {
        return;
    }
    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.IniFilename = nullptr; // window positions are not kept
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
    io.BackendFlags |= ImGuiBackendFlags_HasGamepad;
    io.BackendPlatformName = "spartanport";

    ImGui::StyleColorsDark();
    ImGuiStyle& style = ImGui::GetStyle();
    style.WindowRounding = 6.0f;
    style.FrameRounding = 4.0f;
    style.GrabRounding = 4.0f;
    style.Colors[ImGuiCol_WindowBg].w = 0.92f; style.Colors[ImGuiCol_Text] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.0f, 1.0f, 0.0f, 1.0f); style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.0f, 0.55f, 0.0f, 1.0f);

    ImFontConfig font_config;
    font_config.FontDataOwnedByAtlas = false;
    io.Fonts->AddFontFromMemoryTTF(const_cast<unsigned char*>(spartanport_font_ttf),
                                   int(spartanport_font_ttf_end - spartanport_font_ttf), 18.0f, &font_config);

    const vk::Instance vk_instance = instance.GetInstance();
    struct LoaderContext {VkInstance instance;VkDevice device;};
    LoaderContext loader{vk_instance,static_cast<VkDevice>(instance.GetDevice())};
    const bool functions_loaded=ImGui_ImplVulkan_LoadFunctions(
        std::min(instance.ApiVersion(),Vulkan::TargetVulkanApiVersion),
        [](const char* name, void* user) {
            auto& loader=*static_cast<LoaderContext*>(user);
            auto proc=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetDeviceProcAddr(loader.device,name);
            if(!proc) proc=VULKAN_HPP_DEFAULT_DISPATCHER.vkGetInstanceProcAddr(loader.instance,name);
#ifdef GOW3_UWP
            // Dozen's UWP build has no surface or swapchain functions. The overlay draws into the presenter's
            // images and never uses imgui's window helpers, but imgui refuses to start with any function missing.
            if(!proc&&(std::strstr(name,"Surface")||std::strstr(name,"Swapchain")))
                return reinterpret_cast<PFN_vkVoidFunction>(+[] {
                    std::fputs("Overlay: imgui called a Vulkan window helper, which UWP lacks\n",stderr); std::abort(); });
#endif
            if(!proc) std::fprintf(stderr,"Overlay: Vulkan function unavailable: %s\n",name);
            return proc;
        },
        &loader);
    if(!functions_loaded) {
        std::puts("Overlay: required functions unavailable, disabled");
        ImGui::DestroyContext();return;
    }

    const VkFormat color_format = static_cast<VkFormat>(format);
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = std::min(instance.ApiVersion(),Vulkan::TargetVulkanApiVersion);
    info.Instance = vk_instance;
    info.PhysicalDevice = instance.GetPhysicalDevice();
    info.Device = instance.GetDevice();
    info.QueueFamily = instance.GetGraphicsQueueFamilyIndex();
    info.Queue = instance.GetGraphicsQueue();
    // Font atlas plus the save list pictures (up to 33, and a closed list's until freed).
    info.DescriptorPoolSize = 128;
    info.MinImageCount = std::max(image_count, 2u);
    info.ImageCount = std::max(image_count, 2u);
    info.UseDynamicRendering = true;
    info.PipelineInfoMain.PipelineRenderingCreateInfo = {
        .sType = VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO_KHR,
        .colorAttachmentCount = 1,
        .pColorAttachmentFormats = &color_format,
    };
    if (!ImGui_ImplVulkan_Init(&info)) {
        std::printf("Overlay: ImGui Vulkan backend init failed\n");
        ImGui::DestroyContext();
        return;
    }
    overlay_instance = &instance;
    initialized = true;
    std::printf("Overlay: menu ready (Insert or R3+L2)\n");
}

void UpdateTextInput(SDL_Window* window) {
    bool want = false;
    {
        std::scoped_lock lock{imgui_mutex};
        want = initialized && menu_open && ImGui::GetIO().WantTextInput;
    }
    if (want != SDL_TextInputActive(window)) {
        if (want) {
            SDL_StartTextInput(window);
        } else {
            SDL_StopTextInput(window);
        }
    }
}

// imgui_mutex held. The backend frees a texture some frames after its last use.
void RetireChoiceTextures() {
    for (ImTextureData* tex : choice_textures) {
        if (tex) {
            tex->WantDestroyNextFrame = true;
            choice_retired.push_back(tex);
        }
    }
    choice_textures.clear();
    choice_pictures.clear();
    choice_retiring = !choice_retired.empty();
}

// imgui_mutex held, before NewFrame: new pictures become textures, destroyed ones are freed.
void UpdateChoiceTextures() {
    if (!choice_pictures.empty()) {
        choice_textures.assign(choice_pictures.size(), nullptr);
        for (size_t i = 0; i < choice_pictures.size(); ++i) {
            const SpartanPng::Image& picture = choice_pictures[i];
            if (picture.rgba.empty()) {
                continue;
            }
            ImTextureData* tex = IM_NEW(ImTextureData)();
            tex->Create(ImTextureFormat_RGBA32, picture.width, picture.height);
            std::memcpy(tex->GetPixels(), picture.rgba.data(), picture.rgba.size());
            ImGui::RegisterUserTexture(tex);
            choice_textures[i] = tex;
        }
        choice_pictures.clear();
    }
    std::erase_if(choice_retired, [](ImTextureData* tex) {
        if (tex->Status != ImTextureStatus_Destroyed) {
            return false;
        }
        ImGui::UnregisterUserTexture(tex);
        IM_DELETE(tex);
        return true;
    });
    choice_retiring = !choice_retired.empty();
}

// The save list takes the keys and buttons that move and confirm; the rest stays with the game
// (held neutral by CapturesInput).
bool HandleChoiceEvent(const SDL_Event& event) {
    int move = 0, finish = 0; // finish: 1 accept, 2 cancel
    if (event.type == SDL_EVENT_KEY_DOWN) {
        const SDL_Keycode key = event.key.key;
        move = key == SDLK_UP || key == SDLK_W ? -1 : key == SDLK_DOWN || key == SDLK_S ? 1 : 0;
        finish = key == SDLK_RETURN || key == SDLK_KP_ENTER || key == SDLK_SPACE ? 1
                 : key == SDLK_ESCAPE || key == SDLK_BACKSPACE                   ? 2
                                                                                 : 0;
    } else if (event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN) {
        const u8 button = event.gbutton.button;
        move = button == SDL_GAMEPAD_BUTTON_DPAD_UP ? -1 : button == SDL_GAMEPAD_BUTTON_DPAD_DOWN ? 1 : 0;
        finish = button == SDL_GAMEPAD_BUTTON_SOUTH ? 1 : button == SDL_GAMEPAD_BUTTON_EAST ? 2 : 0;
    } else {
        return event.type == SDL_EVENT_KEY_UP || event.type == SDL_EVENT_GAMEPAD_BUTTON_UP;
    }
    const int count = int(choice_items.size());
    if (move && count) {
        choice_index = (choice_index + move + count) % count;
    }
    if (finish) {
        choice_result = finish == 1 && count ? choice_index : -2;
        choice_active = false;
        RetireChoiceTextures();
    }
    return true;
}

bool HandleEvent(const SDL_Event& event) {
    std::scoped_lock lock{imgui_mutex};
    if (!initialized) {
        return false;
    }
    if (choice_active && !menu_open) {
        return HandleChoiceEvent(event);
    }
    ImGuiIO& io = ImGui::GetIO();
    const bool is_open = menu_open;
    switch (event.type) {
    case SDL_EVENT_KEY_DOWN:
    case SDL_EVENT_KEY_UP: {
        const bool down = event.type == SDL_EVENT_KEY_DOWN;
        static const bool capture_key = [] {
            const char* env = std::getenv("SPARTAN_FRAME_CAPTURE_KEY");
            return env && *env == '1';
        }();
        if (event.key.key == SDLK_F9) {
            if (down && !event.key.repeat) {
                CycleEngineFps();
            }
            return true;
        }
        if (event.key.key == SDLK_F12) {
            if (down && !event.key.repeat) {
                VideoCore::RequestScreenshot(VideoCore::ScreenshotRequest::GameOnly);
            }
            return true;
        }
        if (down && !event.key.repeat && capture_key && event.key.key == SDLK_F11) {
            Vulkan::FrameCapture::Request();
            return true;
        }
        if (down && !event.key.repeat &&
            (event.key.key == SDLK_INSERT || (is_open && event.key.key == SDLK_ESCAPE))) {
            SetOpen(event.key.key == SDLK_INSERT ? !is_open : false);
            return true;
        }
        if (!is_open) {
            return false;
        }
        io.AddKeyEvent(ImGuiMod_Ctrl, (event.key.mod & SDL_KMOD_CTRL) != 0);
        io.AddKeyEvent(ImGuiMod_Shift, (event.key.mod & SDL_KMOD_SHIFT) != 0);
        io.AddKeyEvent(ImGuiMod_Alt, (event.key.mod & SDL_KMOD_ALT) != 0);
        if (const ImGuiKey key = KeyFromSdl(event.key.key); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_BUTTON_DOWN:
    case SDL_EVENT_GAMEPAD_BUTTON_UP: {
        const bool down = event.type == SDL_EVENT_GAMEPAD_BUTTON_DOWN;
        const u8 button = event.gbutton.button;
        // R3+L2 opens the menu: L3+R3 is a game action.
        if (button == SDL_GAMEPAD_BUTTON_RIGHT_STICK) {
            r3_down = down;
            if (down && l2_down) {
                SetOpen(!is_open);
                return true;
            }
        }
        if (!is_open) {
            return false;
        }
        if (const ImGuiKey key = KeyFromGamepad(button); key != ImGuiKey_None) {
            io.AddKeyEvent(key, down);
        }
        return true;
    }
    case SDL_EVENT_GAMEPAD_AXIS_MOTION: {
        if (event.gaxis.axis == SDL_GAMEPAD_AXIS_LEFT_TRIGGER) {
            const bool down = event.gaxis.value > 16000;
            if (down && !l2_down && r3_down) {
                l2_down = true;
                SetOpen(!is_open);
                return true;
            }
            l2_down = down;
        }
        return false;
    }
    case SDL_EVENT_TEXT_INPUT: {
        // Typed characters (Ctrl+click on a slider, a text field): key events alone erase but
        // do not type. SDL sends them while text input is on (UpdateTextInput).
        if (!is_open) {
            return false;
        }
        io.AddInputCharactersUTF8(event.text.text);
        return true;
    }
    case SDL_EVENT_MOUSE_MOTION: {
        if (!is_open) {
            return false;
        }
        const float density = PixelDensity(event.motion.windowID);
        io.AddMousePosEvent(event.motion.x * density, event.motion.y * density);
        return true;
    }
    case SDL_EVENT_MOUSE_BUTTON_DOWN:
    case SDL_EVENT_MOUSE_BUTTON_UP: {
        if (!is_open) {
            return false;
        }
        const int button = event.button.button == SDL_BUTTON_LEFT    ? 0
                           : event.button.button == SDL_BUTTON_RIGHT  ? 1
                           : event.button.button == SDL_BUTTON_MIDDLE ? 2
                                                                      : -1;
        if (button >= 0) {
            io.AddMouseButtonEvent(button, event.type == SDL_EVENT_MOUSE_BUTTON_DOWN);
        }
        return true;
    }
    case SDL_EVENT_MOUSE_WHEEL:
        if (!is_open) {
            return false;
        }
        io.AddMouseWheelEvent(event.wheel.x, event.wheel.y);
        return true;
    default:
        return false;
    }
}

// After "Apply and restart", the new options are kept once the game has shown about ten
// seconds of frames after loading (run.py restores the previous ones otherwise).
void ConfirmRestart() {
    static u32 frames = 0;
    auto& s = SpartanSettings::Get();
    if (!s.restart_unconfirmed || loading || ++frames < 600) {
        return;
    }
    s.restart_unconfirmed = false;
    SpartanSettings::Save();
    std::puts("Settings: graphics options confirmed after restart");
}

void SetBackgroundLoading(u32 done, u32 total) {
    background_done = done;
    background_total = total;
}

void SetLoading(bool active, u32 done, u32 total) {
    loading_done = done;
    loading_total = total;
    loading = active;
}
void SetLoadingLabel(const std::string& label) {
    std::scoped_lock lock{imgui_mutex};
    loading_label = label;
}

bool Visible() {
    const auto& s = SpartanSettings::Get();
    return initialized && (loading || background_total || menu_open || text_entry_active || choice_active ||
#ifdef GOW3_UWP
                           Gow3Uwp::Overlay::Visible() ||
#endif
                           choice_retiring || s.show_fps || NoticeActive() ||
                           (s.actor_watch_supported && s.enemy_health_bar));
}

bool CapturesInput() {
    return menu_open || text_entry_active || choice_active;
}

void BeginChoice(const std::string& title, const std::vector<std::string>& items,
                 std::vector<SpartanPng::Image> pictures, int focus) {
    std::scoped_lock lock{imgui_mutex};
    RetireChoiceTextures();
    choice_pictures = std::move(pictures);
    choice_pictures.resize(items.size());
    choice_title = title;
    choice_items = items;
    choice_index = focus >= 0 && focus < int(items.size()) ? focus : 0;
    choice_result = -1;
    choice_active = true;
}

int PollChoice() {
    return choice_result;
}

void SetTextEntry(bool active, const std::string& prompt, const std::string& text) {
    std::scoped_lock lock{imgui_mutex};
    text_entry_active = active;
    text_entry_prompt = prompt;
    text_entry_text = text;
}

void Render(vk::CommandBuffer cmdbuf, vk::ImageView view, vk::Extent2D extent) {
    // Present interval for the FPS readout (measured also while nothing is drawn).
    const auto now = std::chrono::steady_clock::now();
    const float ms = std::chrono::duration<float, std::milli>(now - last_present).count();
    last_present = now;
    if (ms > 0.0f && ms < 1000.0f) {
        frame_ms_avg = frame_ms_avg == 0.0f ? ms : frame_ms_avg * 0.95f + ms * 0.05f;
    }
    ConfirmRestart();
    if (!Visible()) {
        return;
    }
    std::scoped_lock lock{imgui_mutex};
    ImGuiIO& io = ImGui::GetIO();
    io.DisplaySize = ImVec2(float(extent.width), float(extent.height));
    io.DeltaTime = ms > 0.0f && ms < 1000.0f ? ms / 1000.0f : 1.0f / 60.0f;
    // UI scale follows the display height (1080p = 1).
    const float scale = std::max(float(extent.height) / 1080.0f, 0.75f);
    if (std::abs(scale - base_scale) > 0.01f) {
        ImGuiStyle& style = ImGui::GetStyle();
        style.ScaleAllSizes(scale / base_scale);
        style.FontScaleMain = scale;
        base_scale = scale;
    }

    UpdateChoiceTextures();
    ImGui_ImplVulkan_NewFrame();
    ImGui::NewFrame();
#ifdef GOW3_UWP
    Gow3Uwp::Overlay::Render(*overlay_instance);
#endif
    if (loading) {
        LoadingScreen();
    }
    if (menu_open) {
        Menu();
    }
    if (SpartanSettings::Get().show_fps && !menu_open) {
        FpsCounter();
    }
    if (NoticeActive()) {
        Notice();
    }
    if (!loading && !menu_open) {
        HealthDisplay();
    }
    if (background_total && !loading && !menu_open) {
        BackgroundLoading();
    }
    if (text_entry_active) {
        TextEntryBox();
    }
    if (choice_active && !menu_open) {
        ChoiceBox();
    }
    ImGui::Render();

    const vk::RenderingAttachmentInfo attachment{
        .imageView = view,
        .imageLayout = vk::ImageLayout::eColorAttachmentOptimal,
        .loadOp = vk::AttachmentLoadOp::eLoad,
        .storeOp = vk::AttachmentStoreOp::eStore,
    };
    cmdbuf.beginRendering(vk::RenderingInfo{
        .renderArea = {{0, 0}, extent},
        .layerCount = 1,
        .colorAttachmentCount = 1,
        .pColorAttachments = &attachment,
    });
    {
        // Font atlas uploads submit to the graphics queue themselves.
        std::scoped_lock submit_lock{Vulkan::Scheduler::submit_mutex};
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmdbuf);
    }
    cmdbuf.endRendering();
}

} // namespace SpartanOverlay
