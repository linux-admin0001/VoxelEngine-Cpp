#include "UiDocument.h"

#include "gui/UINode.h"
#include "gui/panels.h"
#include "InventoryView.h"
#include "../logic/scripting/scripting.h"
#include "../files/files.h"
#include "../frontend/gui/gui_xml.h"

UiDocument::UiDocument(
    std::string id, 
    uidocscript script, 
    std::shared_ptr<gui::UINode> root,
    int env
) : id(id), script(script), root(root), env(env) {
    collect(map, root);
}


const uinodes_map& UiDocument::getMap() const {
    return map;
}

const std::string& UiDocument::getId() const {
    return id;
}

const std::shared_ptr<gui::UINode> UiDocument::getRoot() const {
    return root;
}

const uidocscript& UiDocument::getScript() const {
    return script;
}

int UiDocument::getEnvironment() const {
    return env;
}

void UiDocument::collect(uinodes_map& map, std::shared_ptr<gui::UINode> node) {
    const std::string& id = node->getId();
    if (!id.empty()) {
        map[id] = node;
    }
    auto container = dynamic_cast<gui::Container*>(node.get());
    if (container) {
        for (auto subnode : container->getNodes()) {
            collect(map, subnode);
        }
    }
}

std::unique_ptr<UiDocument> UiDocument::read(int env, std::string namesp, fs::path file) {
    const std::string text = files::read_string(file);
    auto xmldoc = xml::parse(file.u8string(), text);
    gui::UiXmlReader reader(env);
    InventoryView::createReaders(reader);
    auto view = reader.readXML(
        file.u8string(), xmldoc->getRoot()
    );
    uidocscript script {};
    auto scriptFile = fs::path(file.u8string()+".lua");
    if (fs::is_regular_file(scriptFile)) {
        scripting::load_layout_script(env, namesp, scriptFile, script);
    }
    return std::make_unique<UiDocument>(namesp, script, view, env);
}
