#include "NativeHud.h"
#include <QFileInfo>
#include <algorithm>

/** 文件夹管理 */
void NativeHud::localPanel(double x, double y, double width) {
    const auto &local = *m_music->local;
    const auto left = x + 53;
    const auto usable = width - 106;
    if (local.busy) {
        button("取消扫描", left, y + 200, 170, 44, "local:cancel", 15);
    } else {
        button("＋ 导入文件夹", left, y + 200, 180, 44, "local:folder", 15);
        button("导入文件", left + 195, y + 200, 135, 44, "local:files", 15);
        button("重新扫描", left + 345, y + 200, 145, 44, "local:rescan", 15);
    }
    text(QStringLiteral("已索引 %1 首 / %2 个导入位置").arg(local.songs().size()).arg(local.roots().size()),
         left, y + 273, 16, "demibold");
    const auto roots = local.roots();
    listOffset = std::min(listOffset, std::max(0, int(roots.size()) - 4));
    for (int row = 0; row < 4 && row + listOffset < roots.size(); ++row) {
        const auto index = row + listOffset;
        const auto root = roots.at(index).toObject();
        const auto top = y + 320 + row * 82;
        const auto path = root["path"].toString();
        rule(left, top - 12, usable, 0.2);
        limited(root["name"].toString(), left, top, 20, usable - 135, "demibold");
        limited((QFileInfo(path).isReadable() ? QString() : QStringLiteral("无法访问 / ")) + path, left,
                top + 32, 12, usable - 135);
        if (!local.busy) {
            button("移除索引", left + usable - 110, top + 4, 110, 40,
                   "local:remove:" + QString::number(index), 13);
        }
    }
    if (roots.isEmpty()) {
        text("选择音乐文件夹，递归导入其中的音乐。", left, y + 360, 20);
        text("只建立索引，音乐文件保留在原位置。", left, y + 405, 16);
    }
    limited(local.status, left, y + 682, 13, usable);
    text("本地标签与封面优先；缺失歌词或封面时自动匹配。", left, y + 718, 12);
    button("浏览全部歌曲 →", left, y + 767, 230, 40, "local:browse", 15);
    if (listOffset > 0) {
        button("上一页", left + usable - 215, y + 767, 100, 40, "local:prev", 13);
    }
    if (listOffset + 4 < roots.size()) {
        button("下一页", left + usable - 105, y + 767, 100, 40, "local:next", 13);
    }
}
