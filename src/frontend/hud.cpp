#include "hud.h"

#include <iostream>
#include <sstream>
#include <memory>
#include <string>
#include <assert.h>
#include <stdexcept>
#include <GL/glew.h>
#include <GLFW/glfw3.h>

#include "../typedefs.h"
#include "../content/Content.h"
#include "../util/stringutil.h"
#include "../util/timeutil.h"
#include "../assets/Assets.h"
#include "../graphics/Shader.h"
#include "../graphics/Batch2D.h"
#include "../graphics/Batch3D.h"
#include "../graphics/Font.h"
#include "../graphics/Atlas.h"
#include "../graphics/Mesh.h"
#include "../window/Camera.h"
#include "../window/Window.h"
#include "../window/Events.h"
#include "../window/input.h"
#include "../voxels/Chunks.h"
#include "../voxels/Block.h"
#include "../world/World.h"
#include "../world/Level.h"
#include "../objects/Player.h"
#include "../physics/Hitbox.h"
#include "../maths/voxmaths.h"
#include "gui/controls.h"
#include "gui/panels.h"
#include "gui/UINode.h"
#include "gui/GUI.h"
#include "ContentGfxCache.h"
#include "screens.h"
#include "WorldRenderer.h"
#include "BlocksPreview.h"
#include "InventoryView.h"
#include "LevelFrontend.h"
#include "UiDocument.h"
#include "../engine.h"
#include "../core_defs.h"
#include "../items/ItemDef.h"
#include "../items/Inventory.h"
#include "../logic/scripting/scripting.h"

using glm::vec2;
using glm::vec3;
using glm::vec4;
using namespace gui;

static std::shared_ptr<Label> create_label(gui::wstringsupplier supplier) {
    auto label = std::make_shared<Label>(L"-");
    label->textSupplier(supplier);
    return label;
}

std::shared_ptr<UINode> HudRenderer::createDebugPanel(Engine* engine) {
    auto level = frontend->getLevel();

    auto panel = std::make_shared<Panel>(vec2(250, 200), vec4(5.0f), 2.0f);
    panel->listenInterval(0.5f, [this]() {
        fpsString = std::to_wstring(fpsMax)+L" / "+std::to_wstring(fpsMin);
        fpsMin = fps;
        fpsMax = fps;
    });
    panel->setCoord(vec2(10, 10));
    panel->add(create_label([this](){ return L"fps: "+this->fpsString;}));
    panel->add(create_label([](){
        return L"meshes: " + std::to_wstring(Mesh::meshesCount);
    }));
    panel->add(create_label([=](){
        auto& settings = engine->getSettings();
        bool culling = settings.graphics.frustumCulling;
        return L"frustum-culling: "+std::wstring(culling ? L"on" : L"off");
    }));
    panel->add(create_label([=]() {
        return L"chunks: "+std::to_wstring(level->chunks->chunksCount)+
               L" visible: "+std::to_wstring(level->chunks->visible);
    }));
    panel->add(create_label([=](){
        auto player = level->player;
        auto* indices = level->content->getIndices();
        auto def = indices->getBlockDef(player->selectedVoxel.id);
        std::wstringstream stream;
        stream << std::hex << level->player->selectedVoxel.states;
        if (def) {
            stream << L" (" << util::str2wstr_utf8(def->name) << L")";
        }
        return L"block: "+std::to_wstring(player->selectedVoxel.id)+
               L" "+stream.str();
    }));
    panel->add(create_label([=](){
        return L"seed: "+std::to_wstring(level->world->getSeed());
    }));

    for (int ax = 0; ax < 3; ax++){
        auto sub = std::make_shared<Panel>(vec2(10, 27), vec4(0.0f));
        sub->setOrientation(Orientation::horizontal);

        std::wstring str = L"x: ";
        str[0] += ax;
        auto label = std::make_shared<Label>(str);
        label->setMargin(vec4(2, 3, 2, 3));
        sub->add(label);
        sub->setColor(vec4(0.0f));

        // Coord input
        auto box = std::make_shared<TextBox>(L"");
        box->textSupplier([=]() {
            Hitbox* hitbox = level->player->hitbox.get();
            return util::to_wstring(hitbox->position[ax], 2);
        });
        box->textConsumer([=](std::wstring text) {
            try {
                vec3 position = level->player->hitbox->position;
                position[ax] = std::stoi(text);
                level->player->teleport(position);
            } catch (std::invalid_argument& _){
            }
        });
        box->setOnEditStart([=](){
            Hitbox* hitbox = level->player->hitbox.get();
            box->setText(std::to_wstring(int(hitbox->position[ax])));
        });

        sub->add(box);
        panel->add(sub);
    }
    panel->add(create_label([=](){
        int hour, minute, second;
        timeutil::from_value(level->world->daytime, hour, minute, second);

        std::wstring timeString = 
                     util::lfill(std::to_wstring(hour), 2, L'0') + L":" +
                     util::lfill(std::to_wstring(minute), 2, L'0');
        return L"time: "+timeString;
    }));
    {
        auto bar = std::make_shared<TrackBar>(0.0f, 1.0f, 1.0f, 0.005f, 8);
        bar->supplier([=]() {return level->world->daytime;});
        bar->consumer([=](double val) {level->world->daytime = val;});
        panel->add(bar);
    }
    {
        auto bar = std::make_shared<TrackBar>(0.0f, 1.0f, 0.0f, 0.005f, 8);
        bar->supplier([=]() {return WorldRenderer::fog;});
        bar->consumer([=](double val) {WorldRenderer::fog = val;});
        panel->add(bar);
    }
    {
        auto checkbox = std::make_shared<FullCheckBox>(
            L"Show Chunk Borders", vec2(400, 24)
        );
        checkbox->supplier([=]() {
            return engine->getSettings().debug.showChunkBorders;
        });
        checkbox->consumer([=](bool checked) {
            engine->getSettings().debug.showChunkBorders = checked;
        });
        panel->add(checkbox);
    }
    panel->refresh();
    return panel;
}

std::shared_ptr<InventoryView> HudRenderer::createContentAccess() {
    auto level = frontend->getLevel();
    auto content = level->content;
    auto indices = content->getIndices();
    auto player = level->player;
    auto inventory = player->getInventory();
    
    int itemsCount = indices->countItemDefs();
    auto accessInventory = std::make_shared<Inventory>(0, itemsCount);
    for (int id = 1; id < itemsCount; id++) {
        accessInventory->getSlot(id-1).set(ItemStack(id, 1));
    }

    SlotLayout slotLayout(-1, glm::vec2(), false, true, 
    [=](ItemStack& item) {
        auto copy = ItemStack(item);
        inventory->move(copy, indices);
    }, 
    [=](ItemStack& item, ItemStack& grabbed) {
        inventory->getSlot(player->getChosenSlot()).set(item);
    });

    InventoryBuilder builder;
    builder.addGrid(8, itemsCount-1, glm::vec2(), 8, true, slotLayout);
    auto view = builder.build();
    view->bind(accessInventory, frontend, interaction.get());
    return view;
}

std::shared_ptr<InventoryView> HudRenderer::createHotbar() {
    auto level = frontend->getLevel();
    auto player = level->player;
    auto inventory = player->getInventory();

    SlotLayout slotLayout(-1, glm::vec2(), false, false, nullptr, nullptr);
    InventoryBuilder builder;
    builder.addGrid(10, 10, glm::vec2(), 4, true, slotLayout);
    auto view = builder.build();

    view->setOrigin(glm::vec2(view->getSize().x/2, 0));
    view->bind(inventory, frontend, interaction.get());
    view->setInteractive(false);
    return view;
}

HudRenderer::HudRenderer(Engine* engine, LevelFrontend* frontend) 
    : assets(engine->getAssets()), 
      gui(engine->getGUI()),
      frontend(frontend)
{
    auto menu = gui->getMenu();

    interaction = std::make_unique<InventoryInteraction>();
    grabbedItemView = std::make_shared<SlotView>(
        SlotLayout(-1, glm::vec2(), false, false, nullptr, nullptr)
    );
    grabbedItemView->bind(
        interaction->getGrabbedItem(), 
        frontend, 
        interaction.get()
    );
    grabbedItemView->setColor(glm::vec4());
    grabbedItemView->setInteractive(false);

    contentAccess = createContentAccess();
    contentAccessPanel = std::make_shared<Panel>(
        contentAccess->getSize(), vec4(0.0f), 0.0f
    );
    contentAccessPanel->setColor(glm::vec4());
    contentAccessPanel->add(contentAccess);
    contentAccessPanel->setScrollable(true);

    hotbarView = createHotbar();
    darkOverlay = std::make_unique<Panel>(glm::vec2(4000.0f));
    darkOverlay->setColor(glm::vec4(0, 0, 0, 0.5f));

    uicamera = std::make_unique<Camera>(vec3(), 1);
    uicamera->perspective = false;
    uicamera->flipped = true;

    debugPanel = createDebugPanel(engine);
    menu->reset();
    
    gui->addBack(darkOverlay);
    gui->addBack(hotbarView);
    gui->add(debugPanel);
    gui->add(contentAccessPanel);
    gui->add(grabbedItemView);
}

HudRenderer::~HudRenderer() {
    gui->remove(grabbedItemView);
    if (inventoryView) {
        gui->remove(inventoryView);
    }
    gui->remove(hotbarView);
    gui->remove(darkOverlay);
    gui->remove(contentAccessPanel);
    gui->remove(debugPanel);
}

void HudRenderer::drawDebug(int fps){
    this->fps = fps;
    fpsMin = min(fps, fpsMin);
    fpsMax = max(fps, fpsMax);
}

void HudRenderer::update(bool visible) {
    auto level = frontend->getLevel();
    auto player = level->player;
    auto menu = gui->getMenu();

    debugPanel->setVisible(player->debug && visible);
    menu->setVisible(pause);

    if (!visible && inventoryOpen) {
        closeInventory();
    }
    if (pause && menu->current().panel == nullptr) {
        pause = false;
    }
    if (Events::jpressed(keycode::ESCAPE) && !gui->isFocusCaught()) {
        if (pause) {
            pause = false;
            menu->reset();
        } else if (inventoryOpen) {
            closeInventory();
        } else {
            pause = true;
            menu->setPage("pause");
        }
    }
    if (visible && Events::jactive(BIND_HUD_INVENTORY)) {
        if (!pause) {
            if (inventoryOpen) {
                closeInventory();
            } else {
                openInventory();
            }
        }
    }
    if ((pause || inventoryOpen) == Events::_cursor_locked) {
        Events::toggleCursor();
    }

    vec2 invSize = contentAccessPanel->getSize();
    contentAccessPanel->setVisible(inventoryOpen);
    contentAccessPanel->setSize(vec2(invSize.x, Window::height));
    hotbarView->setVisible(visible);

    for (int i = keycode::NUM_1; i <= keycode::NUM_9; i++) {
        if (Events::jpressed(i)) {
            player->setChosenSlot(i - keycode::NUM_1);
        }
    }
    if (Events::jpressed(keycode::NUM_0)) {
        player->setChosenSlot(9);
    }
    if (!pause && !inventoryOpen && Events::scroll) {
        int slot = player->getChosenSlot();
        slot = (slot - Events::scroll) % 10;
        if (slot < 0) {
            slot += 10;
        }
        player->setChosenSlot(slot);
    }

    darkOverlay->setVisible(pause);
}

void HudRenderer::openInventory() {
    auto level = frontend->getLevel();
    auto player = level->player;
    auto inventory = player->getInventory();

    inventoryOpen = true;
    gui->remove(grabbedItemView);

    inventoryDocument = assets->getLayout("core:inventory");
    inventoryView = std::dynamic_pointer_cast<InventoryView>(inventoryDocument->getRoot());
    inventoryView->bind(inventory, frontend, interaction.get());
    scripting::on_ui_open(inventoryDocument, inventory.get());

    gui->add(inventoryView);
    gui->add(grabbedItemView);
}

void HudRenderer::closeInventory() {
    scripting::on_ui_close(inventoryDocument, inventoryView->getInventory().get());
    inventoryOpen = false;
    ItemStack& grabbed = interaction->getGrabbedItem();
    grabbed.clear();
    gui->remove(inventoryView);
    inventoryView = nullptr;
    inventoryDocument = nullptr;
}

void HudRenderer::draw(const GfxContext& ctx){
    auto level = frontend->getLevel();

    const Viewport& viewport = ctx.getViewport();
    const uint width = viewport.getWidth();
    const uint height = viewport.getHeight();

    Player* player = level->player;

    uicamera->setFov(height);

    auto batch = ctx.getBatch2D();
    batch->begin();

    Shader* uishader = assets->getShader("ui");
    uishader->use();
    uishader->uniformMatrix("u_projview", uicamera->getProjView());
    
    hotbarView->setCoord(glm::vec2(width/2, height-65));
    hotbarView->setSelected(player->getChosenSlot());

    // Crosshair
    batch->begin();
    if (!pause && Events::_cursor_locked && !level->player->debug) {
        batch->lineWidth(2);
        batch->line(width/2, height/2-6, width/2, height/2+6, 0.2f, 0.2f, 0.2f, 1.0f);
        batch->line(width/2+6, height/2, width/2-6, height/2, 0.2f, 0.2f, 0.2f, 1.0f);
        batch->line(width/2-5, height/2-5, width/2+5, height/2+5, 0.9f, 0.9f, 0.9f, 1.0f);
        batch->line(width/2+5, height/2-5, width/2-5, height/2+5, 0.9f, 0.9f, 0.9f, 1.0f);
    }

    if (inventoryOpen) {
        float caWidth = contentAccess->getSize().x;
        glm::vec2 invSize = inventoryView->getSize();

        float width = viewport.getWidth();

        inventoryView->setCoord(glm::vec2(
            glm::min(width/2-invSize.x/2, width-caWidth-10-invSize.x), 
            height/2-invSize.y/2
        ));
        contentAccessPanel->setCoord(glm::vec2(width-caWidth, 0));
    }
    grabbedItemView->setCoord(glm::vec2(Events::cursor));
    batch->render();
}

bool HudRenderer::isInventoryOpen() const {
    return inventoryOpen;
}

bool HudRenderer::isPause() const {
    return pause;
}
