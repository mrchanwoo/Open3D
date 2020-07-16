// ----------------------------------------------------------------------------
// -                        Open3D: www.open3d.org                            -
// ----------------------------------------------------------------------------
// The MIT License (MIT)
//
// Copyright (c) 2020 www.open3d.org
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files (the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
// FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS
// IN THE SOFTWARE.
// ----------------------------------------------------------------------------

#include "open3d/visualization/gui/TreeView.h"

#include <imgui.h>
#include <list>
#include <unordered_map>
#include <sstream>

#include "open3d/visualization/gui/Theme.h"
#include "open3d/visualization/gui/Util.h"

namespace open3d {
namespace visualization {
namespace gui {

struct TreeView::Impl {
    static TreeView::ItemId g_next_id;

    // Note: use std::list because pointers remain valid, unlike std::vector
    //       which will invalidate pointers when it resizes the underlying
    //       array
    struct Item {
        TreeView::ItemId id = -1;
        std::string text;
        Item *parent = nullptr;
        std::list<Item> children;
    };
    Item root_;
    std::unordered_map<TreeView::ItemId, Item*> id2item_;
    TreeView::ItemId selected_id_ = -1;
    std::function<void(const char*, TreeView::ItemId)> on_value_changed_;
};

TreeView::ItemId TreeView::Impl::g_next_id = 0;

TreeView::TreeView() : impl_(new TreeView::Impl()) {
    impl_->root_.id = Impl::g_next_id++;
    impl_->id2item_[impl_->root_.id] = &impl_->root_;
}

TreeView::~TreeView() {}

TreeView::ItemId TreeView::GetRootItem() const {
    return impl_->root_.id;
}

TreeView::ItemId TreeView::AddItem(ItemId parent_id, const char *text) {
    Impl::Item item;
    item.id = Impl::g_next_id++;
    // ImGUI uses the text to identify the item, so append ##id in case
    // we have multiple items with the same text, as the ID is unique.
    std::stringstream s;
    s << text << "##" << item.id;
    item.text = s.str();

    Impl::Item *parent = &impl_->root_;
    auto parent_it = impl_->id2item_.find(parent_id);
    if (parent_it != impl_->id2item_.end()) {
        parent = parent_it->second;
    }
    item.parent = parent;
    parent->children.push_back(item);
    impl_->id2item_[item.id] = &parent->children.back();

    return item.id;
}

void TreeView::RemoveItem(ItemId item_id) {
    auto item_it = impl_->id2item_.find(item_id);
    if (item_it != impl_->id2item_.end()) {
        auto *item = item_it->second;
        // Erase the item here, because RemoveItem(child) will also erase,
        // which will invalidate our iterator.
        impl_->id2item_.erase(item_it);

        // Remove children. Note that we can't use a foreach loop here,
        // because when we remove the item from its parent it will
        // invalidate the iterator to the current item that exists under
        // the hood, making `it++` not workable. So we use a while loop
        // instead. Because this is a list, we can erase from the front
        // in O(1).
        while (!item->children.empty()) {
            RemoveItem(item->children.front().id);
        }

        // Remove ourself from our parent's list of children
        if (item->parent) {
            for (auto sibling = item->parent->children.begin();
                 sibling != item->parent->children.end();  ++sibling) {
                if (sibling->id == item_id) {
                    item->parent->children.erase(sibling);
                    break;
                }
            }
        }
    }
}

std::vector<TreeView::ItemId> TreeView::GetItemChildren(ItemId parent_id) const {
    std::vector<TreeView::ItemId> children;
    auto item_it = impl_->id2item_.find(parent_id);
    if (item_it != impl_->id2item_.end()) {
        auto *parent = item_it->second->parent;
        if (parent) {
            children.reserve(parent->children.size());
            for (auto &child : parent->children) {
                children.push_back(child.id);
            }
        }
    }
    return children;
}

TreeView::ItemId TreeView::GetSelectedItemId() const {
    if (impl_->selected_id_ < 0) {
        return impl_->root_.id;
    } else {
        return impl_->selected_id_;
    }
}

void TreeView::SetSelectedItemId(ItemId item_id) {
    impl_->selected_id_ = item_id;
}

void TreeView::SetOnValueChanged(std::function<void(const char*, ItemId)> on_value_changed) {
    impl_->on_value_changed_ = on_value_changed;
}

Size TreeView::CalcPreferredSize(const Theme& theme) const {
    return Size(Widget::DIM_GROW, Widget::DIM_GROW);
}

Widget::DrawResult TreeView::Draw(const DrawContext& context) {
    auto &frame = GetFrame();

    // ImGUI's tree can't draw a frame, so we have to do it ourselves
    ImGui::GetWindowDrawList()->AddRectFilled(ImVec2(frame.x, frame.y),
                                              ImVec2(frame.GetRight(), frame.GetBottom()),
                                              colorToImguiRGBA(context.theme.tree_background_color),
                                              context.theme.border_radius);
    ImGui::GetWindowDrawList()->AddRect(ImVec2(frame.x, frame.y),
                                        ImVec2(frame.GetRight(), frame.GetBottom()),
                                        colorToImguiRGBA(context.theme.border_color),
                                        context.theme.border_radius,
                                        ImDrawCornerFlags_All,
                                        context.theme.border_width);

    DrawImGuiPushEnabledState();
    auto x = frame.x - context.uiOffsetX;
    ImGui::SetCursorPosY(frame.y - context.uiOffsetY);
    ImGui::PushItemWidth(frame.width);

    // ImGUI's tree wants to highlight the row as the user moves over it.
    // There are several problems here. First, there seems to be a bug in
    // ImGUI where the highlight ignores the pushed item width and extends
    // to the end of the ImGUI-window (i.e. the topmost parent Widget). This
    // means the highlight extends into any margins we have. Not good. Second,
    // the highlight extends past the clickable area, which is misleading.
    // Third, no operating system has hover highlights like this, and it looks
    // really strange. I mean, you can see the cursor right over your text,
    // what do you need a highligh for? So make this highlight transparent.
    ImGui::PushStyleColor(ImGuiCol_HeaderActive,  // click-hold on item
                          colorToImgui(Color(0, 0, 0, 0)));
    ImGui::PushStyleColor(ImGuiCol_HeaderHovered,
                          colorToImgui(Color(0, 0, 0, 0)));

    Impl::Item *new_selection = nullptr;

    std::function<void(Impl::Item&)> DrawItem;
    DrawItem = [&DrawItem, this, &frame, &theme=context.theme, &new_selection](Impl::Item& item) {
        // ImGUI's tree doesn't seem to support selected items,
        // so we have to draw our own selection.
        if (item.id == impl_->selected_id_) {
            auto h = ImGui::GetTextLineHeightWithSpacing();
            auto y = ImGui::GetCursorPosY();
            ImGui::GetWindowDrawList()->AddRectFilled(
                                ImVec2(frame.x, y),
                                ImVec2(frame.GetRight(), y + h),
                                colorToImguiRGBA(theme.tree_selected_color));
        }

        int flags = ImGuiTreeNodeFlags_DefaultOpen;
        if (item.children.empty()) {
            flags |= ImGuiTreeNodeFlags_Leaf;
        }
        if (ImGui::TreeNodeEx(item.text.c_str(), flags)) {
            if (ImGui::IsItemClicked() && item.children.empty()) {
                impl_->selected_id_ = item.id;
                new_selection = &item;
            }
            for (auto &child : item.children) {
                DrawItem(child);
            }
            ImGui::TreePop();
        } else {
            if (ImGui::IsItemClicked() && item.children.empty()) {
                impl_->selected_id_ = item.id;
                new_selection = &item;
            }
        }
    };
    for (auto &top : impl_->root_.children) {
        // Need to set x for each of the top-level items. Children will
        // take their x position relative to their parent, so we don't want
        // to set their x positions.
        ImGui::SetCursorPosX(x);
        DrawItem(top);
    }

    ImGui::PopStyleColor(2);

    ImGui::PopItemWidth();
    DrawImGuiPopEnabledState();

    // If the selection changed, handle the callback here, after we have
    // finished drawing, so that the callback is able to change the contents
    // of the tree if it wishes (which could cause a crash if done while
    // drawing, e.g. deleting the current item).
    auto result = Widget::DrawResult::NONE;
    if (new_selection) {
        if (impl_->on_value_changed_) {
            impl_->on_value_changed_(new_selection->text.c_str(), new_selection->id);
        }
        result = Widget::DrawResult::REDRAW;
    }

    return result;
}

}  // namespace gui
}  // namespace visualization
}  // namespace open3d

