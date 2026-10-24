// Archive.xapp — Archive manager for FreeBSD-X
//
// Layout:
//   Top bar:    archive name + entry/size summary + Extract Here | Extract To… | New Archive
//   Main area:  scrollable entry list (type icon, path, size)
//   Status bar: operation result / progress message
//
// Features:
//   • Opens ZIP, tar, tar.gz, tar.bz2, tar.xz, tgz, txz — any format libarchive supports
//   • Extract Here: extracts to the archive's parent directory
//   • Extract To…: user picks destination via FileChooser portal
//   • New Archive: user picks a source path → creates <name>.tar.gz alongside it
//   • Drag-and-drop: drop a .zip/.tar.gz/… onto the window to open it
//   • OperationDone broadcast refreshes status bar and re-lists if archive changed
//   • MIME handler: application/zip, application/x-tar, application/x-gzip,
//                   application/x-bzip2, application/x-xz

#include "xkit/widget.h"
#include "xkit/application.h"
#include "xkit/xbus_client.h"
#include "xkit/file_dialog.h"
#include "shared/protocol.h"
#include <memory>
#include <string>
#include <vector>
#include <cstring>
#include <cstdlib>
#include <functional>
#include <algorithm>
#include <sstream>
#include <limits>

using namespace fbx::xkit;
using namespace fbx::shared;

// ── Helpers ───────────────────────────────────────────────────────────────────

static std::string basename(const std::string& path) {
    auto p = path.rfind('/');
    return (p == std::string::npos) ? path : path.substr(p + 1);
}

static std::string dirname(const std::string& path) {
    auto p = path.rfind('/');
    return (p == std::string::npos || p == 0) ? "/" : path.substr(0, p);
}

static bool isArchiveExt(const std::string& path) {
    static const char* exts[] = {
        ".zip", ".tar", ".tar.gz", ".tgz", ".tar.bz2", ".tbz2",
        ".tar.xz", ".txz", ".tar.zst", ".tar.lz4", nullptr
    };
    std::string lower = path;
    std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
    for (int i = 0; exts[i]; ++i)
        if (lower.size() >= std::strlen(exts[i]) &&
            lower.compare(lower.size() - std::strlen(exts[i]),
                          std::strlen(exts[i]), exts[i]) == 0)
            return true;
    return false;
}

static std::string humanSize(uint32_t bytes) {
    if (bytes == 0) return "—";
    if (bytes < 1024)
        return std::to_string(bytes) + " B";
    if (bytes < 1024 * 1024)
        return std::to_string(bytes / 1024) + " KB";
    char buf[16];
    std::snprintf(buf, sizeof(buf), "%.1f MB",
                  static_cast<double>(bytes) / (1024.0 * 1024.0));
    return buf;
}

// ── Local model ───────────────────────────────────────────────────────────────

struct LocalEntry {
    std::string path;
    uint32_t    uncompressed_size{0};
    bool        is_dir{false};
};

static LocalEntry fromArchiveEntry(const ArchiveEntry& e) {
    LocalEntry le;
    le.path              = e.path_;
    le.uncompressed_size = e.uncompressed_size_;
    le.is_dir            = e.is_dir_ != 0;
    return le;
}

// ── App ───────────────────────────────────────────────────────────────────────

int main(int argc, char** argv) {
    // Accept --open <path> or bare positional arg
    std::string open_path;
    for (int i = 1; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--open") { open_path = argv[i+1]; break; }
    }
    if (open_path.empty() && argc == 2 && argv[1][0] != '-')
        open_path = argv[1];

    // ── Shared state ─────────────────────────────────────────────────────────
    auto entries      = std::make_shared<std::vector<LocalEntry>>();
    auto archive_path = std::make_shared<std::string>(open_path);
    auto status_msg   = std::make_shared<std::string>("");
    auto is_loading   = std::make_shared<bool>(false);

    // Widget forward references
    auto list_col    = std::make_shared<std::shared_ptr<Widget>>();
    auto info_lbl    = std::make_shared<std::shared_ptr<Widget>>();
    auto status_lbl  = std::make_shared<std::shared_ptr<Widget>>();
    auto fname_lbl   = std::make_shared<std::shared_ptr<Widget>>();

    // Lambdas — forward-declared
    auto rebuild_list = std::make_shared<std::function<void()>>();
    auto load_page    = std::make_shared<std::function<void(uint32_t)>>();
    auto open_archive = std::make_shared<std::function<void(const std::string&)>>();
    auto set_status   = std::make_shared<std::function<void(const std::string&)>>();

    // ── Root layout: flex column ──────────────────────────────────────
    auto root_col = std::make_shared<FlexColumn>();
    root_col->setFixedSize(750, 520);

    // ══ TOP INFO BAR ══════════════════════════════════════════════════
    auto top_bar = std::make_shared<FlexRow>();
    top_bar->setFixedSize(0.0f, 52);
    top_bar->setPadding(10);
    top_bar->setGap(10);
    top_bar->setBackgroundColor(Color{0.13f, 0.13f, 0.13f, 1.0f});
    top_bar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    root_col->addChild(top_bar);

    // Archive icon + name
    auto icon_lbl = std::make_shared<TextWidget>("🗜", 20.0f);
    top_bar->addChild(icon_lbl);

    auto name_col = std::make_shared<FlexColumn>();
    name_col->setFlexGrow(1.0f);
    name_col->setGap(2);
    top_bar->addChild(name_col);

    auto fnl = std::make_shared<TextWidget>(
        archive_path->empty() ? "No archive open" : basename(*archive_path), 13.0f);
    fnl->setFontWeight(600);
    fnl->setColor(1.0f, 1.0f, 1.0f, 1.0f);
    name_col->addChild(fnl);
    *fname_lbl = fnl;

    auto il = std::make_shared<TextWidget>(
        archive_path->empty() ? "Drag an archive here to open" : "Loading…", 11.0f);
    il->setColor(0.6f, 0.6f, 0.6f, 1.0f);
    name_col->addChild(il);
    *info_lbl = il;

    // ══ TOOLBAR ═══════════════════════════════════════════════════════
    auto toolbar = std::make_shared<FlexRow>();
    toolbar->setFixedSize(0.0f, 38);
    toolbar->setPadding(8);
    toolbar->setGap(6);
    toolbar->setBackgroundColor(Color{0.11f, 0.11f, 0.11f, 1.0f});
    toolbar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    root_col->addChild(toolbar);

    // Extract Here
    auto ext_here_btn = std::make_shared<ButtonWidget>("Extract Here");
    ext_here_btn->setFixedSize(0.0f, 26);
    ext_here_btn->setBackgroundColor(Color{0.18f, 0.48f, 0.18f, 1.0f});
    ext_here_btn->onClick([archive_path, set_status]() {
        if (archive_path->empty()) return;
        std::string dest = dirname(*archive_path);

        ArchiveExtractRequest req{};
        std::strncpy(req.archive_path_, archive_path->c_str(),
                     sizeof(req.archive_path_) - 1);
        std::strncpy(req.dest_path_, dest.c_str(),
                     sizeof(req.dest_path_) - 1);

        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        xbus->Call("com.freebsdx.Archive",
                   static_cast<uint32_t>(ArchiveOpcode::Extract),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                   [set_status](const XBusMessage&) {
                       if (*set_status) (*set_status)("Extracting…");
                   });
    });
    toolbar->addChild(ext_here_btn);

    // Extract To…
    auto ext_to_btn = std::make_shared<ButtonWidget>("Extract To…");
    ext_to_btn->setFixedSize(0.0f, 26);
    ext_to_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});
    ext_to_btn->onClick([archive_path, set_status]() {
        if (archive_path->empty()) return;
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        XFileDialog::openFile(*xbus, "Extract To…", "*/*",
            [archive_path, set_status](const std::string& dest) {
                if (dest.empty()) return;

                ArchiveExtractRequest req{};
                std::strncpy(req.archive_path_, archive_path->c_str(),
                             sizeof(req.archive_path_) - 1);
                std::strncpy(req.dest_path_, dest.c_str(),
                             sizeof(req.dest_path_) - 1);

                auto* xbus2 = XApplication::getBus();
                if (!xbus2) return;
                xbus2->Call("com.freebsdx.Archive",
                            static_cast<uint32_t>(ArchiveOpcode::Extract),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [set_status](const XBusMessage&) {
                                if (*set_status) (*set_status)("Extracting…");
                            });
            });
    });
    toolbar->addChild(ext_to_btn);

    // Divider
    auto div = std::make_shared<FlexRow>();
    div->setFixedSize(1, 22);
    div->setBackgroundColor(Color{0.28f, 0.28f, 0.28f, 1.0f});
    toolbar->addChild(div);

    // New Archive
    auto new_btn = std::make_shared<ButtonWidget>("New Archive…");
    new_btn->setFixedSize(0.0f, 26);
    new_btn->setBackgroundColor(Color{0.22f, 0.22f, 0.22f, 1.0f});
    new_btn->onClick([set_status, open_archive]() {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        XFileDialog::openFile(*xbus, "Select source for archive", "*/*",
            [set_status, open_archive](const std::string& src) {
                if (src.empty()) return;
                std::string dir   = src.rfind('/') == std::string::npos ? "."
                                  : src.substr(0, src.rfind('/'));
                std::string name  = src.rfind('/') == std::string::npos ? src
                                  : src.substr(src.rfind('/') + 1);
                std::string apath = dir + "/" + name + ".tar.gz";

                ArchiveCreateRequest req{};
                std::strncpy(req.archive_path_, apath.c_str(),
                             sizeof(req.archive_path_) - 1);
                std::strncpy(req.source_path_, src.c_str(),
                             sizeof(req.source_path_) - 1);

                auto* xbus2 = XApplication::getBus();
                if (!xbus2) return;
                xbus2->Call("com.freebsdx.Archive",
                            static_cast<uint32_t>(ArchiveOpcode::Create),
                            {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                            [set_status](const XBusMessage&) {
                                if (*set_status) (*set_status)("Creating archive…");
                            });
            });
    });
    toolbar->addChild(new_btn);

    // Spacer
    auto sp = std::make_shared<FlexRow>();
    sp->setFlexGrow(1.0f);
    toolbar->addChild(sp);

    // Open button
    auto open_btn = std::make_shared<ButtonWidget>("Open…");
    open_btn->setFixedSize(0.0f, 26);
    open_btn->setBackgroundColor(Color{0.18f, 0.38f, 0.72f, 1.0f});
    open_btn->onClick([open_archive]() {
        auto* xbus = XApplication::getBus();
        if (!xbus) return;
        XFileDialog::openFile(*xbus, "Open Archive", "*/*",
            [open_archive](const std::string& path) {
                if (!path.empty() && *open_archive) (*open_archive)(path);
            });
    });
    toolbar->addChild(open_btn);

    // ══ ENTRY LIST ════════════════════════════════════════════════════
    auto list_column = std::make_shared<FlexColumn>();
    list_column->setGap(1);
    list_column->setBackgroundColor(Color{0.10f, 0.10f, 0.10f, 1.0f});
    *list_col = list_column;

    auto list_scroll = std::make_shared<ScrollViewWidget>(list_column);
    list_scroll->setFlexGrow(1.0f);
    root_col->addChild(list_scroll);

    // ══ STATUS BAR ════════════════════════════════════════════════════
    auto status_bar = std::make_shared<FlexRow>();
    status_bar->setFixedSize(0.0f, 26);
    status_bar->setPadding(8);
    status_bar->setBackgroundColor(Color{0.11f, 0.11f, 0.11f, 1.0f});
    status_bar->setCrossAxisAlignment(CrossAxisAlignment::Center);
    root_col->addChild(status_bar);

    auto sl = std::make_shared<TextWidget>(
        archive_path->empty() ? "Drag an archive here or click Open…" : "", 11.0f);
    sl->setColor(0.60f, 0.60f, 0.60f, 1.0f);
    status_bar->addChild(sl);
    *status_lbl = sl;

    // ══ SET STATUS LAMBDA ═════════════════════════════════════════════
    *set_status = [=](const std::string& msg) {
        auto sl = *status_lbl;
        if (sl) std::static_pointer_cast<TextWidget>(sl)->setText(msg);
    };

    // ══ REBUILD LIST LAMBDA ═══════════════════════════════════════════
    *rebuild_list = [=]() {
        auto lc = std::static_pointer_cast<FlexColumn>(*list_col);
        if (!lc) return;
        lc->clearChildren();

        if (entries->empty()) {
            auto empty = std::make_shared<TextWidget>(
                archive_path->empty()
                    ? "No archive open.  Drag a .zip/.tar.gz here or click Open…"
                    : "Archive is empty",
                13.0f);
            empty->setColor(0.45f, 0.45f, 0.45f, 1.0f);
            empty->setPadding(20);
            lc->addChild(empty);
            return;
        }

        // Column header
        auto hdr = std::make_shared<FlexRow>();
        hdr->setFixedSize(0.0f, 24);
        hdr->setPadding(8);
        hdr->setGap(8);
        hdr->setBackgroundColor(Color{0.15f, 0.15f, 0.15f, 1.0f});
        lc->addChild(hdr);

        auto hdr_name = std::make_shared<TextWidget>("Name", 11.0f);
        hdr_name->setFontWeight(600);
        hdr_name->setColor(0.65f, 0.65f, 0.65f, 1.0f);
        hdr_name->setFlexGrow(1.0f);
        hdr->addChild(hdr_name);

        auto hdr_size = std::make_shared<TextWidget>("Size", 11.0f);
        hdr_size->setFontWeight(600);
        hdr_size->setColor(0.65f, 0.65f, 0.65f, 1.0f);
        hdr_size->setFixedSize(70, 0.0f);
        hdr->addChild(hdr_size);

        // Entry rows
        for (const auto& e : *entries) {
            auto row = std::make_shared<FlexRow>();
            row->setFixedSize(0.0f, 28);
            row->setPadding(8);
            row->setGap(8);
            row->setCrossAxisAlignment(CrossAxisAlignment::Center);
            row->setBackgroundColor(Color{0.12f, 0.12f, 0.12f, 1.0f});
            lc->addChild(row);

            // Type icon
            auto icon = std::make_shared<TextWidget>(e.is_dir ? "📁" : "📄", 12.0f);
            icon->setFixedSize(18, 0.0f);
            row->addChild(icon);

            // Path
            auto name_lbl = std::make_shared<TextWidget>(e.path, 12.0f);
            name_lbl->setColor(0.88f, 0.88f, 0.88f, 1.0f);
            name_lbl->setFlexGrow(1.0f);
            row->addChild(name_lbl);

            // Size
            auto size_lbl = std::make_shared<TextWidget>(
                e.is_dir ? "—" : humanSize(e.uncompressed_size), 11.0f);
            size_lbl->setColor(0.55f, 0.55f, 0.55f, 1.0f);
            size_lbl->setFixedSize(70, 0.0f);
            row->addChild(size_lbl);
        }

        // Update info label
        uint64_t total_bytes = 0;
        uint32_t file_count  = 0;
        for (const auto& e : *entries) {
            if (!e.is_dir) {
                total_bytes += e.uncompressed_size;
                ++file_count;
            }
        }
        auto info = *info_lbl;
        if (info) {
            std::string s = std::to_string(file_count) + " files · "
                          + humanSize(static_cast<uint32_t>(
                                std::min<uint64_t>(total_bytes,
                                    std::numeric_limits<uint32_t>::max())));
            std::static_pointer_cast<TextWidget>(info)->setText(s);
        }
    };

    // ══ LOAD PAGE LAMBDA ══════════════════════════════════════════════
    *load_page = [=](uint32_t page) {
        if (archive_path->empty()) return;
        auto* xbus = XApplication::getBus();
        if (!xbus) return;

        ArchiveListRequest req{};
        req.page_ = page;
        std::strncpy(req.path_, archive_path->c_str(), sizeof(req.path_) - 1);

        xbus->Call("com.freebsdx.Archive",
                   static_cast<uint32_t>(ArchiveOpcode::List),
                   {reinterpret_cast<const uint8_t*>(&req), sizeof(req)},
                   [entries, rebuild_list, load_page, set_status](const XBusMessage& resp) {
                       ArchiveListPage lp{};
                       std::memcpy(&lp, resp.payload_, sizeof(lp));

                       if (lp.count_ > 0)
                           entries->push_back(fromArchiveEntry(lp.entry_));

                       if (*rebuild_list) (*rebuild_list)();

                       if (lp.has_more_) {
                           if (*load_page) (*load_page)(lp.page_ + 1);
                       } else if (lp.total_ > 0) {
                           if (*set_status)
                               (*set_status)(std::to_string(lp.total_) + " entries");
                       }
                   });
    };

    // ══ OPEN ARCHIVE LAMBDA ═══════════════════════════════════════════
    *open_archive = [=](const std::string& path) {
        *archive_path = path;
        entries->clear();

        auto fl = *fname_lbl;
        if (fl) std::static_pointer_cast<TextWidget>(fl)->setText(basename(path));

        auto info = *info_lbl;
        if (info) std::static_pointer_cast<TextWidget>(info)->setText("Loading…");

        if (*load_page) (*load_page)(0);
    };

    // ══ INITIAL LOAD + BROADCAST + DRAG-DROP (run in init callback) ═══
    return XApplication::Run(root_col, "/tmp/freebsdx.sock", false, false,
        [=](XBusClient& xbus) {
            XApplication::setTitle("Archive");

            // Broadcast listener
            xbus.OnBroadcast([=](const XBusMessage& bcast) {
                // OperationDone
                if (std::strcmp(bcast.service_name_, "com.freebsdx.Archive") == 0 &&
                    bcast.opcode_ == static_cast<uint32_t>(ArchiveOpcode::OperationDone)) {
                    ArchiveStatusResponse sr{};
                    std::memcpy(&sr, bcast.payload_, sizeof(sr));
                    sr.message_[sizeof(sr.message_) - 1] = '\0';
                    if (*set_status) (*set_status)(sr.message_);
                }

                // FileChooser response
                XFileDialog::handleBroadcast(bcast);
            });

            // Drag-and-drop: archives
            XApplication::setOnDrop("text/uri-list",
                [open_archive](float /*x*/, float /*y*/, const std::string& uri) {
                    std::string path = uri;
                    if (path.starts_with("file://"))
                        path = path.substr(7);
                    while (!path.empty() && (path.back() == '\n' || path.back() == '\r'))
                        path.pop_back();
                    if (isArchiveExt(path) && *open_archive) (*open_archive)(path);
                });

            // Initial load
            if (!archive_path->empty() && *open_archive)
                (*open_archive)(*archive_path);
        });
}
