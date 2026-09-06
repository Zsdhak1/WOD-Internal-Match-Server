from __future__ import annotations

import json
from pathlib import Path
from typing import Iterable

from PIL import Image, ImageDraw, ImageFont


ROOT = Path(__file__).resolve().parents[1]
ASSET_DIR = ROOT / "Assets" / "未处理UI"
OUT_DIR = ASSET_DIR / "annotations"


def item(asset_id: str, label: str, box: tuple[int, int, int, int],
         status: str, use: str, note: str = "") -> dict:
    x1, y1, x2, y2 = box
    return {
        "id": asset_id,
        "label": label,
        "bbox": [x1, y1, x2, y2],
        "size": [x2 - x1, y2 - y1],
        "status": status,
        "projectUse": use,
        "note": note,
    }


NOTEPAD_COMPONENTS = [
    item("N01", "数字徽章 1", (58, 23, 88, 63), "optional", "可选",
         "可作为回合/编号数字，但本项目已用文本显示 1v1 与 60 秒。"),
    item("N02", "圆形数字徽章 3", (6, 51, 40, 91), "unused", "不使用",
         "原页面回合/比分类数字，不是机器人状态必需元素。"),
    item("N03", "Round-1 标签", (243, 71, 310, 87), "unused", "不使用"),
    item("N04", "Round-5 标签", (59, 75, 127, 91), "unused", "不使用"),
    item("N05", "Round-4 标签", (151, 75, 219, 91), "unused", "不使用"),
    item("N06", "Round-3 标签", (263, 99, 331, 115), "unused", "不使用"),
    item("N07", "Round-2 标签", (263, 127, 331, 143), "unused", "不使用"),
    item("N08", "比分数字 2", (5, 103, 41, 144), "optional", "可选",
         "可作为比分数字贴图；当前项目比分仍建议使用可缩放文字。"),
    item("N09", "比分数字 0", (56, 103, 92, 143), "optional", "可选"),
    item("N10", "比分数字 3", (107, 103, 143, 143), "optional", "可选"),
    item("N11", "比分数字 2", (159, 103, 193, 144), "optional", "可选"),
    item("N12", "比分数字 0", (209, 103, 245, 143), "optional", "可选"),
    item("N13", "比分数字 7", (353, 104, 382, 142), "optional", "可选"),
    item("N14", "顶部水平边框条", (6, 143, 406, 151), "optional", "可选/改作",
         "可以改作队名或计时器的装饰边框。"),
    item("N15", "中心底座/面板背景", (7, 156, 407, 250), "optional", "可选/改作",
         "可以改作中央比赛标题、计时器或比分面板背景。"),
    item("N16A", "小局积分背景（右侧素材）", (421, 133, 503, 194), "direct",
         "直接使用/右侧蓝方小局积分",
         "图集上方的 N16A 作为右侧素材，斜面朝向中央；叠加当前蓝方小局积分并绘制在 N15 前方。"),
    item("N16B", "小局积分背景（左侧素材）", (421, 194, 503, 256), "direct",
         "直接使用/左侧红方小局积分",
         "图集下方的 N16B 作为左侧素材，斜面朝向中央；叠加当前红方小局积分并绘制在 N15 前方。"),
]


STATUS_LABELS = [
    ("红牌图标", "direct", "直接使用/红牌", "对应裁判事件中的红牌状态。"),
    ("绿色功能图标", "unused", "不使用", "原页面的功能/资源状态图标。"),
    ("红色状态胶囊", "optional", "可选/状态提示", "可作为状态或受击提示图标。"),
    ("蓝边橙色等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("金星等级徽章", "unused", "不使用", "原页面等级徽章。"),
    ("绿色斜向功能图标", "unused", "不使用", "原页面功能图标。"),
    ("红色 B 状态图标", "unused", "不使用", "原页面功能/资源图标。"),
    ("红色钩形图标", "unused", "不使用", "原页面功能或攻击资源图标。"),
    ("机器人热量条", "direct", "直接使用/机器人热量", "对应裁判系统的机器人热量值，不是 HP。"),
    ("黄色分段竖向计量条", "unused", "不使用", "原页面弹药/资源计量元素。"),
    ("黄色斜条图标", "unused", "不使用", "原页面功能图标。"),
    ("黄色感叹号告警牌", "optional", "可选/受击提示", "可作为受击或异常状态提示。"),
    ("白星蓝边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星红边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星青边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("绿色斜条图标", "unused", "不使用", "原页面功能图标。"),
    ("黄牌图标", "direct", "直接使用/黄牌", "对应裁判事件中的黄牌状态。"),
    ("深色竖向蓝色仪表", "unused", "不使用", "原页面资源/装甲仪表。"),
    ("白星三层箭头徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("红色钩形图标", "unused", "不使用", "原页面功能或攻击资源图标。"),
    ("橙色箭头等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("浅青细横向进度条", "unused", "不使用", "另一种资源条变体；机器人热量使用 S09。"),
    ("白星红边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("橙色箭头等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星红边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星蓝色三层箭头徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星红色三层箭头徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星青色三层箭头徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星青色三层箭头徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("橙色箭头等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("蓝色机器人头像", "optional", "可选/机器人头像", "1v1 可选一张作为蓝方机器人身份图。"),
    ("白星红边等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("蓝色横向状态条", "direct", "直接使用/蓝方血条", "可作为蓝方 HP 条或其底图。"),
    ("白星蓝色等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星红色等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("白星青色等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("空圆形槽位蓝边", "unused", "不使用", "原页面未填充槽位。"),
    ("蓝色机器人头像", "optional", "可选/机器人头像", "可作为蓝方机器人身份图。"),
    ("蓝色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("红色横向状态条", "direct", "直接使用/红方血条", "可作为红方 HP 条或其底图。"),
    ("青色护盾/能量图标", "unused", "不使用", "本项目当前不显示护盾/能量。"),
    ("红色机器人头像", "optional", "可选/机器人头像", "可作为红方机器人身份图。"),
    ("深色红边机器人头像", "optional", "可选/机器人头像", "可作为红方机器人身份图。"),
    ("白色多边形标签板", "optional", "可选/队名背景", "可改作队名覆盖层的装饰底板。"),
    ("红色机器人头像", "optional", "可选/机器人头像", "可作为红方机器人身份图。"),
    ("蓝色机器人头像", "optional", "可选/机器人头像", "可作为蓝方机器人身份图。"),
    ("红色机器人头像", "optional", "可选/机器人头像", "可作为红方机器人身份图。"),
    ("蓝色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("红色机器人头像", "optional", "可选/机器人头像", "可作为红方机器人身份图。"),
    ("空圆形槽位红边", "unused", "不使用", "原页面未填充槽位。"),
    ("蓝色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("红色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("绿色横向状态条", "unused", "不使用", "当前项目不显示该类资源状态。"),
    ("橙色箭头等级徽章", "unused", "不使用", "原页面等级/兵种徽章。"),
    ("洋红色横向状态条", "unused", "不使用", "原页面其他阵营/状态条。"),
    ("红色横向状态条", "direct", "直接使用/红方血条", "可作为红方 HP 条紧凑变体。"),
    ("蓝色横向状态条", "direct", "直接使用/蓝方血条", "可作为蓝方 HP 条紧凑变体。"),
    ("灰色三角形侧板", "unused", "不使用", "原页面布局装饰。"),
    ("蓝色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("红色机器人头像金色选中圈", "optional", "可选/机器人头像", "金色圈可改作在线/当前焦点状态。"),
    ("洋红色横向状态条", "unused", "不使用", "原页面其他阵营/状态条。"),
    ("超长红色发光血条框", "direct", "直接使用/红方血条", "最适合做大屏红方主血条。"),
    ("蓝色横向状态条", "direct", "直接使用/蓝方血条", "可作为蓝方主血条或紧凑变体。"),
]

STATUS_BOXES = [
    (4, 269, 57, 335), (71, 286, 113, 333), (208, 343, 256, 399),
    (104, 345, 192, 433), (4, 346, 92, 434), (273, 359, 309, 399),
    (323, 362, 357, 400), (569, 400, 607, 444), (211, 410, 487, 434),
    (643, 413, 675, 510), (509, 442, 551, 490), (957, 442, 998, 543),
    (104, 445, 192, 533), (204, 445, 292, 533), (4, 446, 92, 534),
    (594, 456, 636, 504), (897, 472, 950, 538), (398, 476, 439, 577),
    (304, 483, 392, 571), (1003, 493, 1041, 537), (493, 502, 581, 590),
    (607, 523, 871, 531), (104, 545, 192, 633), (204, 545, 292, 633),
    (4, 546, 92, 634), (1059, 549, 1147, 637), (1159, 549, 1247, 637),
    (1259, 549, 1347, 637), (960, 550, 1046, 636), (860, 551, 946, 637),
    (331, 582, 478, 711), (1403, 597, 1491, 685), (491, 600, 848, 640),
    (1721, 602, 1809, 690), (1821, 602, 1909, 690), (1921, 602, 2009, 690),
    (1591, 632, 1707, 748), (170, 643, 317, 777), (4, 646, 156, 777),
    (1035, 647, 1392, 687), (1503, 648, 1537, 686), (714, 649, 861, 780),
    (875, 649, 1022, 777), (1715, 695, 1924, 761), (1266, 696, 1413, 827),
    (1427, 696, 1574, 822), (553, 711, 700, 845), (387, 714, 539, 845),
    (1105, 720, 1252, 846), (1721, 764, 1837, 880), (772, 779, 924, 913),
    (939, 782, 1091, 913), (2, 783, 374, 839), (1851, 794, 1939, 882),
    (1334, 828, 1706, 884), (2, 851, 375, 907), (386, 851, 759, 907),
    (1136, 855, 1318, 1018), (1720, 892, 1872, 1024), (1887, 895, 2039, 1024),
    (1334, 896, 1707, 952), (18, 931, 1105, 1010), (1334, 964, 1707, 1020),
]


REFERENCE_REGIONS = [
    item("R01", "左方队伍名称与队徽", (150, 22, 1110, 170), "repurpose", "需要/改作",
         "本项目保留队名文字，队徽不是必需。"),
    item("R02", "左方基地/前哨站血量", (160, 42, 430, 200), "repurpose", "改作机器人血量",
         "本项目不显示基地或前哨站，使用红方机器人 HP。"),
    item("R03", "左方兵种/机器人卡列", (30, 165, 1010, 345), "repurpose", "改作单个红方机器人卡",
         "1v1 只保留一张红方机器人信息卡。"),
    item("R04", "中央回合、比分、计时器", (1060, 0, 1510, 190), "direct", "直接使用/简化",
         "保留比分和 60 秒计时；去除 Round 1/5。"),
    item("R05", "右方队伍名称与队徽", (1510, 22, 2410, 170), "repurpose", "需要/改作",
         "本项目保留蓝方队名文字，队徽可选。"),
    item("R06", "右方基地/前哨站血量", (2180, 42, 2530, 200), "repurpose", "改作机器人血量",
         "本项目不显示基地或前哨站，使用蓝方机器人 HP。"),
    item("R07", "右方兵种/机器人卡列", (1540, 165, 2530, 345), "repurpose", "改作单个蓝方机器人卡",
         "1v1 只保留一张蓝方机器人信息卡。"),
    item("R08", "场内目标/据点状态条", (780, 415, 1680, 1030), "unused", "不使用",
         "当前赛事转播不需要前哨站、基地、场内目标血量。"),
    item("R09", "中央事件/资源提示", (1070, 165, 1500, 370), "unused", "不使用",
         "不需要能量、资源和地图事件提示。"),
    item("R10", "左下解说员覆盖层", (80, 1190, 960, 1405), "unused", "不使用",
         "暂不做解说员信息。"),
    item("R11", "右下小地图", (1940, 1080, 2540, 1405), "unused", "不使用",
         "当前项目无地图数据源。"),
    item("R12", "底部赛事标题条", (1030, 1260, 1550, 1438), "optional", "可选",
         "可改作赛事名称或比赛状态。"),
]


CENTER_HUD_REFERENCE_COMPONENTS = [
    item("C01", "左侧 N16B 计分板（左侧素材）", (10, 9, 67, 56), "repurpose",
         "需要/左侧覆盖层",
         "N16B 的斜面朝向中央；绘制在 N15 前方并压住 N15 左边缘。"),
    item("C02", "中央 N15 计时底座", (61, 8, 151, 66), "direct",
         "直接使用/中央计时背景",
         "N15 位于左右 N16 之间，作为计时器和比赛比分的底座。"),
    item("C03", "右侧 N16A 计分板（右侧素材）", (142, 9, 196, 56), "repurpose",
         "需要/右侧覆盖层",
         "N16A 的斜面朝向中央；绘制在 N15 前方并压住 N15 右边缘。"),
    item("C04", "回合文字 Round 1/5", (70, 0, 135, 11), "unused", "不使用",
         "项目固定为 1v1、60 秒，不显示原 UI 的回合标签。"),
    item("C05", "左侧比分数字 0", (28, 23, 51, 47), "optional", "可选/左方小局积分",
         "项目使用动态文字叠加到左侧 N16B。"),
    item("C06", "中央比赛计时 6:57", (75, 23, 137, 49), "direct", "直接使用/60 秒倒计时",
         "项目显示 01:00 到 00:00 的实时比赛计时。"),
    item("C07", "右侧比分数字 0", (159, 23, 183, 47), "optional", "可选/右方小局积分",
         "项目使用动态文字叠加到右侧 N16A。"),
    item("C08", "中央底部高亮条", (82, 51, 125, 60), "repurpose", "可选/N15 装饰",
         "属于 N15 的底部高亮细节，不单独作为业务字段。"),
    item("C09", "右下黄色装饰图标", (167, 53, 181, 67), "unused", "不使用",
         "与比赛状态无关，不放入导播台 HUD。"),
]


def load_font(size: int):
    candidates = [
        Path("C:/Windows/Fonts/msyh.ttc"),
        Path("C:/Windows/Fonts/msyhbd.ttc"),
        Path("C:/Windows/Fonts/arial.ttf"),
        Path("C:/Windows/Fonts/consola.ttf"),
    ]
    for candidate in candidates:
        if candidate.exists():
            try:
                return ImageFont.truetype(str(candidate), size)
            except OSError:
                pass
    return ImageFont.load_default()


def draw_annotations(source: Path, output: Path, components: Iterable[dict],
                     title: str) -> None:
    image = Image.open(source).convert("RGBA")
    draw = ImageDraw.Draw(image, "RGBA")
    font = load_font(max(12, min(22, image.width // 90)))
    colors = {
        "direct": (30, 205, 100, 255),
        "optional": (255, 185, 45, 255),
        "repurpose": (255, 145, 45, 255),
        "unused": (240, 70, 75, 255),
    }

    for component in components:
        x1, y1, x2, y2 = component["bbox"]
        color = colors.get(component["status"], (255, 255, 255, 255))
        draw.rectangle((x1, y1, x2 - 1, y2 - 1), outline=color, width=3)
        label = component["id"]
        bbox = draw.textbbox((0, 0), label, font=font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        lx = max(0, min(x1, image.width - tw - 8))
        ly = max(0, y1 - th - 6)
        draw.rounded_rectangle((lx, ly, lx + tw + 8, ly + th + 4), radius=3,
                               fill=(0, 0, 0, 220), outline=color, width=1)
        draw.text((lx + 4, ly + 1), label, fill=(255, 255, 255, 255), font=font)

    # Add a compact legend in unused transparent space when possible.
    legend_font = load_font(max(12, min(18, image.width // 120)))
    legend = [("绿色 直接使用", colors["direct"]),
              ("黄色 可选/改作", colors["optional"]),
              ("红色 当前不使用", colors["unused"])]
    if image.width >= 800:
        x, y = image.width - 300, 12
        box_h = 26 * len(legend) + 18
        draw.rounded_rectangle((x - 10, y - 8, image.width - 12, y + box_h),
                               radius=5, fill=(0, 0, 0, 205), outline=(255, 255, 255, 150), width=1)
        for index, (text, color) in enumerate(legend):
            yy = y + index * 26
            draw.rectangle((x, yy + 4, x + 16, yy + 20), fill=color)
            draw.text((x + 24, yy), text, fill=(255, 255, 255, 255), font=legend_font)

    output.parent.mkdir(parents=True, exist_ok=True)
    image.save(output)


def draw_scaled_center_reference(source: Path, output: Path,
                                 components: list[dict], scale: int = 4) -> None:
    source_image = Image.open(source).convert("RGBA")
    source_image = source_image.resize((source_image.width * scale,
                                        source_image.height * scale),
                                       Image.Resampling.LANCZOS)
    image_offset = (20, 74)
    legend_width = 420
    canvas = Image.new("RGBA",
                       (image_offset[0] + source_image.width + legend_width,
                        image_offset[1] + source_image.height + 96),
                       (12, 16, 21, 255))
    canvas.alpha_composite(source_image, image_offset)
    draw = ImageDraw.Draw(canvas, "RGBA")
    title_font = load_font(21)
    label_font = load_font(17)
    detail_font = load_font(15)
    colors = {
        "direct": (30, 205, 100, 255),
        "optional": (255, 185, 45, 255),
        "repurpose": (255, 145, 45, 255),
        "unused": (240, 70, 75, 255),
    }

    draw.text((20, 20),
              "中央计时 HUD 参考图标注（坐标按原图 203×67）",
              fill=(245, 248, 250, 255), font=title_font)
    draw.rectangle((image_offset[0] - 1,
                    image_offset[1] - 1,
                    image_offset[0] + source_image.width,
                    image_offset[1] + source_image.height),
                   outline=(160, 180, 190, 255), width=2)

    # Put labels outside the most crowded top edge, and inside the remaining
    # boxes where the enlarged image has enough room.
    top_labels = {"C01": (10, 1), "C02": (116, 1), "C03": (142, 1), "C04": (70, 0)}
    for component in components:
        x1, y1, x2, y2 = component["bbox"]
        color = colors.get(component["status"], (255, 255, 255, 255))
        box = (image_offset[0] + x1 * scale,
               image_offset[1] + y1 * scale,
               image_offset[0] + x2 * scale - 1,
               image_offset[1] + y2 * scale - 1)
        draw.rectangle(box, outline=color, width=3)

        if component["id"] in top_labels:
            lx, ly = top_labels[component["id"]]
            lx = image_offset[0] + lx * scale
            ly = max(44, image_offset[1] + ly * scale - 27)
        elif component["id"] == "C09":
            lx = image_offset[0] + x1 * scale
            ly = image_offset[1] + y2 * scale + 5
        else:
            lx = image_offset[0] + x1 * scale + 4
            ly = image_offset[1] + y1 * scale + 4

        bbox = draw.textbbox((0, 0), component["id"], font=label_font)
        tw, th = bbox[2] - bbox[0], bbox[3] - bbox[1]
        draw.rounded_rectangle((lx, ly, lx + tw + 12, ly + th + 8),
                               radius=4, fill=(0, 0, 0, 225), outline=color, width=2)
        draw.text((lx + 6, ly + 3), component["id"],
                  fill=(255, 255, 255, 255), font=label_font)

    legend_x = image_offset[0] + source_image.width + 30
    legend_y = 74
    draw.text((legend_x, 20), "组件与排版关系", fill=(245, 248, 250, 255), font=title_font)
    for index, component in enumerate(components):
        y = legend_y + index * 36
        color = colors.get(component["status"], (255, 255, 255, 255))
        draw.rectangle((legend_x, y + 5, legend_x + 18, y + 23), fill=color)
        draw.text((legend_x + 28, y),
                  f"{component['id']}  {component['label']}",
                  fill=(235, 240, 243, 255), font=detail_font)

    output.parent.mkdir(parents=True, exist_ok=True)
    canvas.convert("RGB").save(output)


def write_manifest(path: Path, image_name: str, size: tuple[int, int], components: list[dict]) -> None:
    payload = {
        "image": image_name,
        "size": list(size),
        "coordinateConvention": "[x1, y1, x2, y2), origin at top-left; x2/y2 are exclusive",
        "components": components,
    }
    path.write_text(json.dumps(payload, ensure_ascii=False, indent=2), encoding="utf-8")


def main() -> None:
    OUT_DIR.mkdir(parents=True, exist_ok=True)

    notepad_source = ASSET_DIR / "sactx-0-512x256-DXT5_BC3-NotepadAtlas-a8efa592.png"
    notepad = Image.open(notepad_source)
    draw_annotations(notepad_source, OUT_DIR / "annotated_notepad_atlas.png",
                     NOTEPAD_COMPONENTS, "Notepad Atlas")
    write_manifest(OUT_DIR / "notepad_atlas.json", notepad_source.name,
                   notepad.size, NOTEPAD_COMPONENTS)

    status_source = ASSET_DIR / "sactx-0-2048x1024-DXT5_BC3-StatusbarAtlas-be4c2c94.png"
    status = Image.open(status_source)
    status_components = [
        item(f"S{index:02d}", label, box, status_value, use, note)
        for index, (box, (label, status_value, use, note))
        in enumerate(zip(STATUS_BOXES, STATUS_LABELS), 1)
    ]
    draw_annotations(status_source, OUT_DIR / "annotated_statusbar_atlas.png",
                     status_components, "Statusbar Atlas")
    write_manifest(OUT_DIR / "statusbar_atlas.json", status_source.name,
                   status.size, status_components)

    reference_source = Path(r"C:/Users/hhh20/AppData/Local/Temp/codex-clipboard-a26bcf9c-8516-4610-8b78-4bbde124c479.png")
    reference = Image.open(reference_source)
    draw_annotations(reference_source, OUT_DIR / "annotated_reference_ui.png",
                     REFERENCE_REGIONS, "Reference UI")
    write_manifest(OUT_DIR / "reference_ui.json", reference_source.name,
                   reference.size, REFERENCE_REGIONS)

    center_reference_source = Path(
        r"C:/Users/hhh20/AppData/Local/Temp/codex-clipboard-b2369f50-6f00-494a-be30-13461a66e321.png"
    )
    if center_reference_source.exists():
        center_reference = Image.open(center_reference_source)
        draw_scaled_center_reference(center_reference_source,
                                     OUT_DIR / "annotated_center_hud_reference.png",
                                     CENTER_HUD_REFERENCE_COMPONENTS)
        write_manifest(OUT_DIR / "center_hud_reference.json",
                       center_reference_source.name,
                       center_reference.size,
                       CENTER_HUD_REFERENCE_COMPONENTS)

    print(f"Wrote annotations to {OUT_DIR}")


if __name__ == "__main__":
    main()
