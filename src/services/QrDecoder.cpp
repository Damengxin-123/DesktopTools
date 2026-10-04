#include "QrDecoder.h"

#include <QStringDecoder>
#include <QByteArray>
#include <QPointF>
#include <algorithm>
#include <array>
#include <cmath>
#include <vector>

namespace QrDecoder {
namespace {

using std::size_t;

// 统计置位数量（C++17 无 <bit>，自行实现）。
int bitCount(int value)
{
    int count = 0;
    while (value != 0) {
        value &= value - 1;
        ++count;
    }
    return count;
}

constexpr int MaximumVersion = 40;
constexpr int MaximumSize = MaximumVersion * 4 + 17; // 177 个模块。
// 解码前限制像素数量，控制二值化与模块采样的内存和耗时。
constexpr qint64 MaximumDecodePixels = 12 * 1000 * 1000;
constexpr int MaximumFinderCandidates = 40;
constexpr int MaximumTripleCandidates = 12;
// 每次解码尝试的几何假设数量上限（按拟合度排序取最优的若干个）。
constexpr int MaximumHypotheses = 10;
constexpr int MaximumSegments = 128;
constexpr int MaximumTextLength = 10000;
// 1:1:3:1:1 定位图案各段允许的行程长度偏差（以单模块宽度为单位）。
constexpr double UnitTolerance = 0.5;
constexpr double CenterTolerance = 0.8;
// 格式信息 BCH(15,5) 允许的最大位错误数，可唯一纠正到 32 个合法值之一。
constexpr int MaximumFormatErrors = 3;

// ---------- 灰度准备与二值化 ----------

// 缩小超大图片后转为 8 位灰度，后续所有步骤只使用该副本。
QImage prepareGray(const QImage& image)
{
    qint64 pixels = qint64(image.width()) * image.height();
    QImage source = image;
    if (pixels > MaximumDecodePixels) {
        const double factor = std::sqrt(double(MaximumDecodePixels) / double(pixels));
        source = image.scaled(std::max(1, int(image.width() * factor)),
            std::max(1, int(image.height() * factor)), Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
    }
    return source.convertToFormat(QImage::Format_Grayscale8);
}

// 全局 Otsu 阈值：截图等双峰分布图片的最佳分割点。
int otsuThreshold(const QImage& gray)
{
    std::array<int, 256> histogram{};
    for (int y = 0; y < gray.height(); ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < gray.width(); ++x)
            ++histogram[line[x]];
    }
    const qint64 total = qint64(gray.width()) * gray.height();
    qint64 sumAll = 0;
    for (int value = 0; value < 256; ++value)
        sumAll += qint64(value) * histogram[value];
    qint64 sumDark = 0;
    qint64 weightDark = 0;
    double bestVariance = -1.0;
    int threshold = 127;
    for (int value = 0; value < 256; ++value) {
        weightDark += histogram[value];
        if (weightDark <= 0)
            continue;
        const qint64 weightLight = total - weightDark;
        if (weightLight <= 0)
            break;
        sumDark += qint64(value) * histogram[value];
        const double meanDark = double(sumDark) / double(weightDark);
        const double meanLight = double(sumAll - sumDark) / double(weightLight);
        const double between = double(weightDark) * double(weightLight) * (meanDark - meanLight) * (meanDark - meanLight);
        if (between > bestVariance) {
            bestVariance = between;
            threshold = value;
        }
    }
    return threshold;
}

struct BinaryImage {
    int width = 0;
    int height = 0;
    std::vector<quint8> dark; // 行优先；1 表示深色像素。
    bool at(int x, int y) const { return dark[size_t(y) * width + x] != 0; }
};

BinaryImage binarizeGlobal(const QImage& gray, int threshold)
{
    BinaryImage result;
    result.width = gray.width();
    result.height = gray.height();
    result.dark.resize(size_t(result.width) * result.height);
    for (int y = 0; y < result.height; ++y) {
        const uchar* line = gray.constScanLine(y);
        for (int x = 0; x < result.width; ++x)
            result.dark[size_t(y) * result.width + x] = line[x] <= threshold ? 1 : 0;
    }
    return result;
}

// 块均值自适应二值化：块边长随图片缩放，应对光照不均的照片。
BinaryImage binarizeAdaptive(const QImage& gray)
{
    BinaryImage result;
    result.width = gray.width();
    result.height = gray.height();
    result.dark.resize(size_t(result.width) * result.height);
    const int block = std::max(8, std::min(result.width, result.height) / 24);
    for (int tileY = 0; tileY < result.height; tileY += block) {
        for (int tileX = 0; tileX < result.width; tileX += block) {
            const int right = std::min(tileX + block, result.width);
            const int bottom = std::min(tileY + block, result.height);
            qint64 sum = 0;
            for (int y = tileY; y < bottom; ++y) {
                const uchar* line = gray.constScanLine(y);
                for (int x = tileX; x < right; ++x)
                    sum += line[x];
            }
            const qint64 count = qint64(right - tileX) * (bottom - tileY);
            // 阈值略微偏向深色，保护抗锯齿边缘处的黑白判定；纯深色块内保持下限为 1，
            // 避免阈值变成负数后把黑色模块内部判成白色。
            const int mean = int(sum / count);
            const int threshold = std::max(1, mean - std::max(3, mean / 20));
            for (int y = tileY; y < bottom; ++y) {
                const uchar* line = gray.constScanLine(y);
                for (int x = tileX; x < right; ++x)
                    result.dark[size_t(y) * result.width + x] = line[x] < threshold ? 1 : 0;
            }
        }
    }
    return result;
}

// 双线性灰度采样；坐标越界时按边缘像素处理。
double sampleGray(const QImage& gray, double px, double py)
{
    const int width = gray.width();
    const int height = gray.height();
    const double x = std::clamp(px, 0.0, double(width - 1));
    const double y = std::clamp(py, 0.0, double(height - 1));
    const int x0 = int(x);
    const int y0 = int(y);
    const int x1 = std::min(x0 + 1, width - 1);
    const int y1 = std::min(y0 + 1, height - 1);
    const double fx = x - x0;
    const double fy = y - y0;
    const auto* row0 = gray.constScanLine(y0);
    const auto* row1 = gray.constScanLine(y1);
    const double top = row0[x0] * (1.0 - fx) + row0[x1] * fx;
    const double bottom = row1[x0] * (1.0 - fx) + row1[x1] * fx;
    return top * (1.0 - fy) + bottom * fy;
}

// ---------- 定位图案检测 ----------

struct Run {
    int start = 0;
    int length = 0;
    bool dark = false;
};

void rowRuns(const BinaryImage& image, int y, std::vector<Run>& runs)
{
    runs.clear();
    int x = 0;
    while (x < image.width) {
        const bool dark = image.at(x, y);
        int end = x + 1;
        while (end < image.width && image.at(end, y) == dark)
            ++end;
        runs.push_back({x, end - x, dark});
        x = end;
    }
}

void columnRuns(const BinaryImage& image, int x, std::vector<Run>& runs)
{
    runs.clear();
    int y = 0;
    while (y < image.height) {
        const bool dark = image.at(x, y);
        int end = y + 1;
        while (end < image.height && image.at(x, end) == dark)
            ++end;
        runs.push_back({y, end - y, dark});
        y = end;
    }
}

// 五段行程是否符合 1:1:3:1:1；unit 返回估算的单模块像素宽度。
bool finderRatios(const Run* window, double* unit)
{
    double total = 0;
    for (int i = 0; i < 5; ++i)
        total += window[i].length;
    if (total < 7.0)
        return false;
    const double u = total / 7.0;
    if (u < 1.5)
        return false;
    static constexpr double expected[5] = {1.0, 1.0, 3.0, 1.0, 1.0};
    for (int i = 0; i < 5; ++i) {
        const double deviation = std::fabs(window[i].length / u - expected[i]);
        if (deviation > (i == 2 ? 0.7 : UnitTolerance))
            return false;
    }
    *unit = u;
    return true;
}

struct FinderCandidate {
    double x = 0;    // 图案中心的像素坐标。
    double y = 0;
    double unit = 0; // 估算的单模块像素宽度。
};

// 在二值图中按行扫描定位图案，并用纵向行程比例交叉验证。
std::vector<FinderCandidate> findFinders(const BinaryImage& image)
{
    std::vector<FinderCandidate> found;
    std::vector<Run> runs;
    std::vector<Run> column;
    for (int y = 0; y < image.height; ++y) {
        rowRuns(image, y, runs);
        if (runs.size() < 5)
            continue;
        for (size_t i = 0; i + 4 < runs.size(); ++i) {
            const Run* window = &runs[i];
            if (!window[0].dark || window[1].dark || !window[2].dark || window[3].dark || !window[4].dark)
                continue;
            double unit = 0;
            if (!finderRatios(window, &unit))
                continue;
            const double cx = window[2].start + window[2].length / 2.0;
            const double cy = y + 0.5;
            columnRuns(image, std::clamp(int(cx), 0, image.width - 1), column);
            double verticalUnit = 0;
            double verticalCenter = 0;
            for (size_t j = 0; j + 4 < column.size(); ++j) {
                const Run* vertical = &column[j];
                if (!vertical[0].dark || vertical[1].dark || !vertical[2].dark || vertical[3].dark || !vertical[4].dark)
                    continue;
                double candidateUnit = 0;
                if (!finderRatios(vertical, &candidateUnit))
                    continue;
                // 行扫描会在图案中心的整条竖带上多次命中；纵向窗口的中心才是
                // 图案的精确中心，直接采用，避免候选点偏移破坏几何重建。
                const double center = vertical[2].start + vertical[2].length / 2.0;
                if (std::fabs(center - cy) > CenterTolerance * unit)
                    continue;
                if (candidateUnit < unit * 0.7 || candidateUnit > unit * 1.4)
                    continue;
                verticalUnit = candidateUnit;
                verticalCenter = center;
                break;
            }
            if (verticalUnit <= 0)
                continue;
            found.push_back({cx, verticalCenter, (unit + verticalUnit) / 2.0});
        }
    }
    // 合并相邻的重复候选，优先保留模块更大的（更清晰的）图案。
    std::sort(found.begin(), found.end(), [](const FinderCandidate& a, const FinderCandidate& b) {
        return a.unit > b.unit;
    });
    std::vector<FinderCandidate> unique;
    for (const FinderCandidate& candidate : found) {
        bool duplicate = false;
        for (const FinderCandidate& kept : unique) {
            const double dx = candidate.x - kept.x;
            const double dy = candidate.y - kept.y;
            if (dx * dx + dy * dy < candidate.unit * candidate.unit * 2.25) {
                duplicate = true;
                break;
            }
        }
        if (!duplicate)
            unique.push_back(candidate);
        if (unique.size() >= MaximumFinderCandidates)
            break;
    }
    return unique;
}

// ---------- 几何重建 ----------

// 模块坐标 → 像素坐标的映射，支持仿射与单应两种模式。
struct Mapper {
    double h[9] = {1, 0, 0, 0, 1, 0, 0, 0, 1}; // 行优先齐次矩阵，h[8] 恒为 1。
    bool perspective = false;

    QPointF map(double x, double y) const
    {
        if (!perspective)
            return QPointF(h[0] * x + h[1] * y + h[2], h[3] * x + h[4] * y + h[5]);
        const double denominator = h[6] * x + h[7] * y + 1.0;
        return QPointF((h[0] * x + h[1] * y + h[2]) / denominator,
            (h[3] * x + h[4] * y + h[5]) / denominator);
    }
};

// 由三个定位图案中心构造精确仿射映射。
Mapper affineFromTriple(const FinderCandidate& a, const FinderCandidate& b,
    const FinderCandidate& c, int size)
{
    const QPointF pa(a.x, a.y);
    const QPointF pb(b.x, b.y);
    const QPointF pc(c.x, c.y);
    const QPointF u = (pb - pa) / double(size - 7);
    const QPointF v = (pc - pa) / double(size - 7);
    const QPointF origin = pa - u * 3.5 - v * 3.5;
    Mapper mapper;
    mapper.h[0] = u.x();
    mapper.h[1] = v.x();
    mapper.h[2] = origin.x();
    mapper.h[3] = u.y();
    mapper.h[4] = v.y();
    mapper.h[5] = origin.y();
    return mapper;
}

// 高斯消元求解四点单应；矩阵奇异时返回 false。
bool solveHomography(const QPointF module[4], const QPointF pixel[4], Mapper& mapper)
{
    double matrix[8][9] = {};
    for (int i = 0; i < 4; ++i) {
        const double x = module[i].x();
        const double y = module[i].y();
        const double u = pixel[i].x();
        const double v = pixel[i].y();
        double* even = matrix[i * 2];
        double* odd = matrix[i * 2 + 1];
        even[0] = x;
        even[1] = y;
        even[2] = 1.0;
        even[6] = -u * x;
        even[7] = -u * y;
        even[8] = u;
        odd[3] = x;
        odd[4] = y;
        odd[5] = 1.0;
        odd[6] = -v * x;
        odd[7] = -v * y;
        odd[8] = v;
    }
    for (int column = 0; column < 8; ++column) {
        int pivot = column;
        for (int row = column + 1; row < 8; ++row) {
            if (std::fabs(matrix[row][column]) > std::fabs(matrix[pivot][column]))
                pivot = row;
        }
        if (std::fabs(matrix[pivot][column]) < 1e-9)
            return false;
        if (pivot != column)
            std::swap(matrix[pivot], matrix[column]);
        for (int row = 0; row < 8; ++row) {
            if (row == column)
                continue;
            const double factor = matrix[row][column] / matrix[column][column];
            for (int k = column; k < 9; ++k)
                matrix[row][k] -= matrix[column][k] * factor;
        }
    }
    Mapper solved;
    solved.perspective = true;
    for (int i = 0; i < 8; ++i)
        solved.h[i] = matrix[i][8] / matrix[i][i];
    mapper = solved;
    return true;
}

struct GeometryHypothesis {
    int a = -1;
    int b = -1;
    int c = -1;
    int size = 0;
    int version = 0;
    double fit = 0; // 越小越可信。
};

// 从候选中枚举“左上、右上、左下”的三角组合与可能的版本。
// 旋转会让行程估算的模块宽度偏大（1/cos 倍），单一版本吸附容易判错版本，
// 因此为每个组合生成 ±2 个版本假设，按拟合度排序后逐个尝试完整解码。
std::vector<GeometryHypothesis> enumerateHypotheses(const std::vector<FinderCandidate>& candidates)
{
    std::vector<GeometryHypothesis> hypotheses;
    const int count = int(std::min(candidates.size(), size_t(MaximumTripleCandidates)));
    for (int a = 0; a < count; ++a) {
        for (int b = 0; b < count; ++b) {
            if (b == a)
                continue;
            for (int c = 0; c < count; ++c) {
                if (c == a || c == b)
                    continue;
                const QPointF pa(candidates[a].x, candidates[a].y);
                const QPointF pb(candidates[b].x, candidates[b].y);
                const QPointF pc(candidates[c].x, candidates[c].y);
                const QPointF ab = pb - pa;
                const QPointF ac = pc - pa;
                const double lab = std::hypot(ab.x(), ab.y());
                const double lac = std::hypot(ac.x(), ac.y());
                // 图像坐标 y 轴向下：cross > 0 时 a 为左上角。
                if (ab.x() * ac.y() - ab.y() * ac.x() <= 0)
                    continue;
                if (lab < lac * 0.72 || lab > lac * 1.38)
                    continue;
                const double cosAngle = (ab.x() * ac.x() + ab.y() * ac.y()) / (lab * lac);
                if (std::fabs(cosAngle) > 0.72)
                    continue;
                const double ua = candidates[a].unit;
                const double ub = candidates[b].unit;
                const double uc = candidates[c].unit;
                // 四个尺寸估计（两条中心距分别除以三个图案的模块宽度）取平均。
                const double estimate = 7.0 + (lab / ua + lab / ub + lac / ua + lac / uc) / 4.0;
                const double unitSpread = (std::fabs(ua - ub) + std::fabs(ua - uc)) / ua;
                const int center = int(std::lround((estimate - 17.0) / 4.0));
                for (int version = center - 2; version <= center + 2; ++version) {
                    if (version < 1 || version > MaximumVersion)
                        continue;
                    const double drift = std::fabs(double(version * 4 + 17) - estimate);
                    if (drift > 6.0)
                        continue;
                    hypotheses.push_back({a, b, c, version * 4 + 17, version, drift + unitSpread});
                }
            }
        }
    }
    std::sort(hypotheses.begin(), hypotheses.end(), [](const GeometryHypothesis& left, const GeometryHypothesis& right) {
        return left.fit < right.fit;
    });
    if (hypotheses.size() > MaximumHypotheses)
        hypotheses.resize(MaximumHypotheses);
    return hypotheses;
}

// 在预测位置附近搜索左下角的 alignments 图案中心（模块坐标）。
bool findAlignmentCenter(const BinaryImage& image, const Mapper& mapper, int size, QPointF* moduleOut)
{
    const double unitX = (mapper.map(1.0, 0.0) - mapper.map(0.0, 0.0)).manhattanLength();
    const double step = std::max(0.25, 0.5 / std::max(1.0, unitX)); // 采样约 0.5 像素精度。
    double bestScore = 0;
    QPointF bestModule;
    for (double oy = -3.0; oy <= 3.0; oy += step) {
        for (double ox = -3.0; ox <= 3.0; ox += step) {
            const double mx = size - 7 + ox;
            const double my = size - 7 + oy;
            int score = 0;
            const auto sample = [&](double dx, double dy, bool expectDark) {
                const QPointF pixel = mapper.map(mx + dx, my + dy);
                const bool dark = image.at(std::clamp(int(pixel.x()), 0, image.width - 1),
                    std::clamp(int(pixel.y()), 0, image.height - 1));
                if (dark == expectDark)
                    ++score;
            };
            sample(0, 0, true);
            for (int ring = 1; ring <= 2; ++ring) {
                const bool dark = ring == 2;
                sample(0, -ring, dark);
                sample(0, ring, dark);
                sample(-ring, 0, dark);
                sample(ring, 0, dark);
                if (ring == 1) {
                    sample(-ring, -ring, dark);
                    sample(-ring, ring, dark);
                    sample(ring, -ring, dark);
                    sample(ring, ring, dark);
                }
            }
            if (score > bestScore) {
                bestScore = score;
                bestModule = QPointF(mx, my);
            }
        }
    }
    // 17 个采样点至多允许 2 个不符，明显偏离对齐图案时放弃矫正。
    if (bestScore < 15)
        return false;
    *moduleOut = bestModule;
    return true;
}

// ---------- 模块矩阵 ----------

struct ModuleGrid {
    int size = 0;
    std::vector<quint8> dark;
    std::vector<quint8> isFunction;
    bool darkAt(int x, int y) const { return dark[size_t(y) * size + x] != 0; }
};

// 对齐图案中心坐标：版本 1 没有对齐图案，其余按规范公式生成。
std::vector<int> alignmentPositions(int version)
{
    if (version == 1)
        return {};
    const int count = version / 7 + 2;
    int step = 0;
    if (version == 32) {
        step = 26;
    } else {
        const int span = count * 2 - 2;
        step = ((version * 4 + 4) + span - 1) / span * 2;
    }
    std::vector<int> positions{6};
    int position = version * 4 + 17 - 7;
    while (int(positions.size()) < count) {
        positions.insert(positions.begin() + 1, position);
        position -= step;
    }
    return positions;
}

void buildFunctionMap(int version, ModuleGrid& grid)
{
    const int size = grid.size;
    const auto mark = [&](int x, int y) {
        if (x >= 0 && x < size && y >= 0 && y < size)
            grid.isFunction[size_t(y) * size + x] = 1;
    };
    // 定位图案与分隔符所在的 8×8 区域。
    for (int offset = 0; offset < 8; ++offset) {
        for (int index = 0; index < 8; ++index) {
            mark(index, offset);
            mark(size - 8 + index, offset);
            mark(offset, size - 8 + index);
        }
    }
    for (int i = 0; i < size; ++i) {
        mark(i, 6); // 时序图案。
        mark(6, i);
    }
    const auto positions = alignmentPositions(version);
    for (const int cy : positions) {
        for (const int cx : positions) {
            if ((cx == 6 && cy == 6) || (cx == size - 7 && cy == 6) || (cx == 6 && cy == size - 7))
                continue;
            for (int dy = -2; dy <= 2; ++dy)
                for (int dx = -2; dx <= 2; ++dx)
                    mark(cx + dx, cy + dy);
        }
    }
    for (int i = 0; i <= 8; ++i) {
        mark(8, i); // 格式信息两份副本及恒黑模块。
        mark(i, 8);
    }
    for (int i = size - 8; i < size; ++i) {
        mark(8, i);
        mark(i, 8);
    }
    if (version >= 7) {
        for (int i = 0; i <= 5; ++i) {
            for (int j = 0; j <= 2; ++j) {
                mark(size - 11 + j, i); // 版本信息两份副本。
                mark(i, size - 11 + j);
            }
        }
    }
}

// 通过几何映射对每个模块多点采样并多数表决。
// binary 在反色尝试中已完成黑白互换，可直接采样；灰度采样路径需要按 inverted 调整判定。
bool sampleModules(const QImage& gray, int otsu, const BinaryImage& binary, bool sampleFromBinary,
    bool inverted, const Mapper& mapper, ModuleGrid& grid)
{
    static constexpr double offsets[3] = {0.3, 0.5, 0.7};
    for (int y = 0; y < grid.size; ++y) {
        for (int x = 0; x < grid.size; ++x) {
            int darkVotes = 0;
            int votes = 0;
            for (const double oy : offsets) {
                for (const double ox : offsets) {
                    const QPointF pixel = mapper.map(x + ox, y + oy);
                    if (pixel.x() < -1.0 || pixel.y() < -1.0
                        || pixel.x() > gray.width() || pixel.y() > gray.height())
                        continue; // 采样点落在图片外（缺少静区）时忽略。
                    ++votes;
                    const bool dark = sampleFromBinary
                        ? binary.at(std::clamp(int(pixel.x()), 0, binary.width - 1),
                              std::clamp(int(pixel.y()), 0, binary.height - 1))
                        : (sampleGray(gray, pixel.x(), pixel.y()) <= otsu) != inverted;
                    if (dark)
                        ++darkVotes;
                }
            }
            if (votes == 0)
                return false;
            grid.dark[size_t(y) * grid.size + x] = darkVotes * 2 >= votes ? 1 : 0;
        }
    }
    return true;
}

// ---------- 格式信息、去掩码与码字提取 ----------

// 32 个合法格式信息位串（含 0x5412 掩码），用于最近匹配纠错。
std::array<int, 32> validFormatValues()
{
    std::array<int, 32> result{};
    size_t index = 0;
    for (const int eccBits : {1, 0, 3, 2}) { // L、M、Q、H。
        for (int mask = 0; mask < 8; ++mask) {
            const int data = eccBits << 3 | mask;
            int remainder = data;
            for (int i = 0; i < 10; ++i)
                remainder = (remainder << 1) ^ ((remainder >> 9) * 0x537);
            result[index++] = ((data << 10) | (remainder & 0x3FF)) ^ 0x5412;
        }
    }
    return result;
}

int readFormatCopy(const ModuleGrid& grid, bool second)
{
    int bits = 0;
    const auto set = [&bits](int index, bool dark) {
        if (dark)
            bits |= 1 << index;
    };
    if (!second) {
        for (int i = 0; i <= 5; ++i)
            set(i, grid.darkAt(8, i));
        set(6, grid.darkAt(8, 7));
        set(7, grid.darkAt(8, 8));
        set(8, grid.darkAt(7, 8));
        for (int i = 9; i <= 14; ++i)
            set(i, grid.darkAt(14 - i, 8));
    } else {
        for (int i = 0; i <= 7; ++i)
            set(i, grid.darkAt(grid.size - 1 - i, 8));
        for (int i = 8; i <= 14; ++i)
            set(i, grid.darkAt(8, grid.size - 15 + i));
    }
    return bits;
}

bool maskBit(int mask, int x, int y)
{
    switch (mask) {
    case 0:
        return (x + y) % 2 == 0;
    case 1:
        return y % 2 == 0;
    case 2:
        return x % 3 == 0;
    case 3:
        return (x + y) % 3 == 0;
    case 4:
        return (x / 3 + y / 2) % 2 == 0;
    case 5:
        return (x * y) % 2 + (x * y) % 3 == 0;
    case 6:
        return ((x * y) % 2 + (x * y) % 3) % 2 == 0;
    default:
        return ((x + y) % 2 + (x * y) % 3) % 2 == 0;
    }
}

// 按“右向左、两列一组、蛇形向下/向上”的顺序读取数据位，并就地去掩码。
std::vector<quint8> collectDataBits(const ModuleGrid& grid, int mask)
{
    std::vector<quint8> bits;
    const int size = grid.size;
    int right = size - 1;
    while (right >= 1) {
        if (right == 6)
            right = 5;
        for (int vert = 0; vert < size; ++vert) {
            for (int j = 0; j < 2; ++j) {
                const int x = right - j;
                const bool upward = ((right + 1) & 2) == 0;
                const int y = upward ? size - 1 - vert : vert;
                if (grid.isFunction[size_t(y) * size + x])
                    continue;
                bool dark = grid.dark[size_t(y) * size + x] != 0;
                if (maskBit(mask, x, y))
                    dark = !dark;
                bits.push_back(dark ? 1 : 0);
            }
        }
        right -= 2;
    }
    return bits;
}

// ---------- 数据段解码 ----------

// RS 块结构：{每块纠错码字, 第一组块数, 第一组每块数据码字, 第二组块数, 第二组每块数据码字}。
// 每个条目满足 块数×(数据+纠错) 之和等于版本总码字（由 tests/tools/verify-rs-table.py 校验）。
struct BlockStructure {
    int ecc = 0;
    int group1Blocks = 0;
    int group1Data = 0;
    int group2Blocks = 0;
    int group2Data = 0;
};

// 纠错级别按格式信息位序索引：0=M、1=L、2=H、3=Q。
const BlockStructure kBlockStructures[MaximumVersion + 1][4] = {
    {},
    {{10, 1, 16, 0, 0}, {7, 1, 19, 0, 0}, {17, 1, 9, 0, 0}, {13, 1, 13, 0, 0}},
    {{16, 1, 28, 0, 0}, {10, 1, 34, 0, 0}, {28, 1, 16, 0, 0}, {22, 1, 22, 0, 0}},
    {{26, 1, 44, 0, 0}, {15, 1, 55, 0, 0}, {22, 2, 13, 0, 0}, {18, 2, 17, 0, 0}},
    {{18, 2, 32, 0, 0}, {20, 1, 80, 0, 0}, {16, 4, 9, 0, 0}, {26, 2, 24, 0, 0}},
    {{24, 2, 43, 0, 0}, {26, 1, 108, 0, 0}, {22, 2, 11, 2, 12}, {18, 2, 15, 2, 16}},
    {{16, 4, 27, 0, 0}, {18, 2, 68, 0, 0}, {28, 4, 15, 0, 0}, {24, 4, 19, 0, 0}},
    {{18, 4, 31, 0, 0}, {20, 2, 78, 0, 0}, {26, 4, 13, 1, 14}, {18, 2, 14, 4, 15}},
    {{22, 2, 38, 2, 39}, {24, 2, 97, 0, 0}, {26, 4, 14, 2, 15}, {22, 4, 18, 2, 19}},
    {{22, 3, 36, 2, 37}, {30, 2, 116, 0, 0}, {24, 4, 12, 4, 13}, {20, 4, 16, 4, 17}},
    {{26, 4, 43, 1, 44}, {18, 2, 68, 2, 69}, {28, 6, 15, 2, 16}, {24, 6, 19, 2, 20}},
    {{30, 1, 50, 4, 51}, {20, 4, 81, 0, 0}, {24, 3, 12, 8, 13}, {28, 4, 22, 4, 23}},
    {{22, 6, 36, 2, 37}, {24, 2, 92, 2, 93}, {28, 7, 14, 4, 15}, {26, 4, 20, 6, 21}},
    {{22, 8, 37, 1, 38}, {26, 4, 107, 0, 0}, {22, 12, 11, 4, 12}, {24, 8, 20, 4, 21}},
    {{24, 4, 40, 5, 41}, {30, 3, 115, 1, 116}, {24, 11, 12, 5, 13}, {20, 11, 16, 5, 17}},
    {{24, 5, 41, 5, 42}, {22, 5, 87, 1, 88}, {24, 11, 12, 7, 13}, {30, 5, 24, 7, 25}},
    {{28, 7, 45, 3, 46}, {24, 5, 98, 1, 99}, {30, 3, 15, 13, 16}, {24, 15, 19, 2, 20}},
    {{28, 10, 46, 1, 47}, {28, 1, 107, 5, 108}, {28, 2, 14, 17, 15}, {28, 1, 22, 15, 23}},
    {{26, 9, 43, 4, 44}, {30, 5, 120, 1, 121}, {28, 2, 14, 19, 15}, {28, 17, 22, 1, 23}},
    {{26, 3, 44, 11, 45}, {28, 3, 113, 4, 114}, {26, 9, 13, 16, 14}, {26, 17, 21, 4, 22}},
    {{26, 3, 41, 13, 42}, {28, 3, 107, 5, 108}, {28, 15, 15, 10, 16}, {30, 15, 24, 5, 25}},
    {{26, 17, 42, 0, 0}, {28, 4, 116, 4, 117}, {30, 19, 16, 6, 17}, {28, 17, 22, 6, 23}},
    {{28, 17, 46, 0, 0}, {28, 2, 111, 7, 112}, {24, 34, 13, 0, 0}, {30, 7, 24, 16, 25}},
    {{28, 4, 47, 14, 48}, {30, 4, 121, 5, 122}, {30, 16, 15, 14, 16}, {30, 11, 24, 14, 25}},
    {{28, 6, 45, 14, 46}, {30, 6, 117, 4, 118}, {30, 30, 16, 2, 17}, {30, 11, 24, 16, 25}},
    {{28, 8, 47, 13, 48}, {26, 8, 106, 4, 107}, {30, 22, 15, 13, 16}, {30, 7, 24, 22, 25}},
    {{28, 19, 46, 4, 47}, {28, 10, 114, 2, 115}, {30, 33, 16, 4, 17}, {28, 28, 22, 6, 23}},
    {{28, 22, 45, 3, 46}, {30, 8, 122, 4, 123}, {30, 12, 15, 28, 16}, {30, 8, 23, 26, 24}},
    {{28, 3, 45, 23, 46}, {30, 3, 117, 10, 118}, {30, 11, 15, 31, 16}, {30, 4, 24, 31, 25}},
    {{28, 21, 45, 7, 46}, {30, 7, 116, 7, 117}, {30, 19, 15, 26, 16}, {30, 1, 23, 37, 24}},
    {{28, 19, 47, 10, 48}, {30, 5, 115, 10, 116}, {30, 23, 15, 25, 16}, {30, 15, 24, 25, 25}},
    {{28, 2, 46, 29, 47}, {30, 13, 115, 3, 116}, {30, 23, 15, 28, 16}, {30, 42, 24, 1, 25}},
    {{28, 10, 46, 23, 47}, {30, 17, 115, 0, 0}, {30, 19, 15, 35, 16}, {30, 10, 24, 35, 25}},
    {{28, 14, 46, 21, 47}, {30, 17, 115, 1, 116}, {30, 11, 15, 46, 16}, {30, 29, 24, 19, 25}},
    {{28, 14, 46, 23, 47}, {30, 13, 115, 6, 116}, {30, 59, 16, 1, 17}, {30, 44, 24, 7, 25}},
    {{28, 12, 47, 26, 48}, {30, 12, 121, 7, 122}, {30, 22, 15, 41, 16}, {30, 39, 24, 14, 25}},
    {{28, 6, 47, 34, 48}, {30, 6, 121, 14, 122}, {30, 2, 15, 64, 16}, {30, 46, 24, 10, 25}},
    {{28, 29, 46, 14, 47}, {30, 17, 122, 4, 123}, {30, 24, 15, 46, 16}, {30, 49, 24, 10, 25}},
    {{28, 13, 46, 32, 47}, {30, 4, 122, 18, 123}, {30, 42, 15, 32, 16}, {30, 48, 24, 14, 25}},
    {{28, 40, 47, 7, 48}, {30, 20, 117, 4, 118}, {30, 10, 15, 67, 16}, {30, 43, 24, 22, 25}},
    {{28, 18, 47, 31, 48}, {30, 19, 118, 6, 119}, {30, 20, 15, 61, 16}, {30, 34, 24, 34, 25}},
};

// 把按块交织的码字流还原为按块顺序拼接的逻辑数据码字序列：
// 块 j 的第 i 个码字位于流中第 (i×总块数 + j) 个数据码字处。
std::vector<quint8> deinterleaveCodewords(const std::vector<quint8>& codewords, const BlockStructure& structure)
{
    const int totalData = structure.group1Blocks * structure.group1Data
        + structure.group2Blocks * structure.group2Data;
    std::vector<quint8> logical;
    if (totalData <= 0 || codewords.size() < size_t(totalData))
        return logical;
    logical.reserve(size_t(totalData));
    const int totalBlocks = structure.group1Blocks + structure.group2Blocks;
    for (int block = 0; block < totalBlocks; ++block) {
        const int size = block < structure.group1Blocks ? structure.group1Data : structure.group2Data;
        for (int column = 0; column < size; ++column)
            logical.push_back(codewords[size_t(column) * totalBlocks + size_t(block)]);
    }
    return logical;
}

// 字节段文本：优先严格 UTF-8，失败时尝试 GB18030，最后按 Latin-1 呈现。
QString decodeByteSpan(const QByteArray& bytes, bool utf8)
{
    if (bytes.isEmpty())
        return QString();
    if (!utf8)
        return QString::fromLatin1(bytes);
    QStringDecoder converter(QStringConverter::Utf8);
    QString text = converter.decode(bytes);
    if (!converter.hasError())
        return text;
    QStringDecoder fallback(QStringLiteral("GB18030"));
    if (fallback.isValid()) {
        text = fallback.decode(bytes);
        if (!fallback.hasError())
            return text;
    }
    return QString::fromLatin1(bytes);
}

const char alphanumericCharset[] = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZ $%*+-./:";

bool decodeSegments(const std::vector<quint8>& codewords, int version, QString* textOut, QString* errorOut)
{
    const size_t totalBits = codewords.size() * 8;
    size_t position = 0;
    const auto readBits = [&](int count, int* value) {
        if (count <= 0 || position + size_t(count) > totalBits)
            return false;
        int result = 0;
        for (int i = 0; i < count; ++i) {
            const size_t index = position + size_t(i);
            result = (result << 1) | int((codewords[index / 8] >> (7 - index % 8)) & 1);
        }
        position += size_t(count);
        *value = result;
        return true;
    };
    QString text;
    bool byteUtf8 = true; // ECI 之后的字节段字符集；默认按 UTF-8 解释。
    const auto fail = [&](const QString& message) {
        *errorOut = message;
        return false;
    };
    const auto appendText = [&](const QString& part) {
        if (text.size() + part.size() > MaximumTextLength)
            return fail(QStringLiteral("二维码内容过长，无法解码。"));
        text += part;
        return true;
    };
    for (int segment = 0; segment < MaximumSegments; ++segment) {
        int mode = 0;
        if (!readBits(4, &mode))
            break; // 剩余不足 4 位：数据已结束（由终止符或填充覆盖）。
        if (mode == 0)
            break; // 终止符。
        if (mode == 1) { // 数字段：3 位数字打包为 10 位。
            int count = 0;
            if (!readBits(version <= 9 ? 10 : (version <= 26 ? 12 : 14), &count))
                return fail(QStringLiteral("二维码数字段的长度信息不完整。"));
            while (count >= 3) {
                int value = 0;
                if (!readBits(10, &value) || value >= 1000)
                    return fail(QStringLiteral("二维码数字段内容无效。"));
                if (!appendText(QString::number(value).rightJustified(3, QLatin1Char('0'))))
                    return false;
                count -= 3;
            }
            if (count == 2) {
                int value = 0;
                if (!readBits(7, &value) || value >= 100)
                    return fail(QStringLiteral("二维码数字段内容无效。"));
                if (!appendText(QString::number(value).rightJustified(2, QLatin1Char('0'))))
                    return false;
            } else if (count == 1) {
                int value = 0;
                if (!readBits(4, &value) || value >= 10)
                    return fail(QStringLiteral("二维码数字段内容无效。"));
                if (!appendText(QString::number(value)))
                    return false;
            } else if (count != 0) {
                return fail(QStringLiteral("二维码数字段长度无效。"));
            }
        } else if (mode == 2) { // 字母数字段：两个字符打包为 11 位。
            int count = 0;
            if (!readBits(version <= 9 ? 9 : (version <= 26 ? 11 : 13), &count))
                return fail(QStringLiteral("二维码字母数字段的长度信息不完整。"));
            while (count >= 2) {
                int value = 0;
                if (!readBits(11, &value) || value >= 45 * 45)
                    return fail(QStringLiteral("二维码字母数字段内容无效。"));
                const QString pair = QString(QLatin1Char(alphanumericCharset[value / 45]))
                    + QLatin1Char(alphanumericCharset[value % 45]);
                if (!appendText(pair))
                    return false;
                count -= 2;
            }
            if (count == 1) {
                int value = 0;
                if (!readBits(6, &value) || value >= 45)
                    return fail(QStringLiteral("二维码字母数字段内容无效。"));
                if (!appendText(QString(QLatin1Char(alphanumericCharset[value]))))
                    return false;
            } else if (count != 0) {
                return fail(QStringLiteral("二维码字母数字段长度无效。"));
            }
        } else if (mode == 4) { // 字节段。
            int count = 0;
            if (!readBits(version <= 9 ? 8 : 16, &count) || count < 0
                || size_t(count) > (totalBits - position) / 8)
                return fail(QStringLiteral("二维码字节段的长度信息不完整。"));
            QByteArray bytes;
            bytes.reserve(count);
            for (int i = 0; i < count; ++i) {
                int value = 0;
                if (!readBits(8, &value))
                    return fail(QStringLiteral("二维码字节段内容不完整。"));
                bytes.append(char(value));
            }
            if (!appendText(decodeByteSpan(bytes, byteUtf8)))
                return false;
        } else if (mode == 7) { // ECI 字符集声明。
            int first = 0;
            if (!readBits(8, &first))
                return fail(QStringLiteral("二维码 ECI 声明不完整。"));
            if (first < 0x80) {
                byteUtf8 = first == 26;
            } else if ((first & 0xC0) == 0x80) {
                int second = 0;
                if (!readBits(8, &second))
                    return fail(QStringLiteral("二维码 ECI 声明不完整。"));
                byteUtf8 = (((first & 0x3F) << 8) | second) == 26;
            } else {
                return fail(QStringLiteral("二维码使用了不支持的 ECI 字符集。"));
            }
        } else {
            return fail(QStringLiteral("二维码包含不支持的数据段类型。"));
        }
    }
    if (text.isEmpty())
        return fail(QStringLiteral("二维码内容为空。"));
    *textOut = text;
    return true;
}

// ---------- 单次尝试 ----------

Result decodeOnce(const QImage& gray, int otsu, bool adaptive, bool inverted)
{
    Result result;
    BinaryImage binary = adaptive ? binarizeAdaptive(gray) : binarizeGlobal(gray, otsu);
    if (inverted) {
        // 反色二维码：黑白互换后定位图案检测照常进行。
        for (quint8& value : binary.dark)
            value = value ? 0 : 1;
    }
    const std::vector<FinderCandidate> finders = findFinders(binary);
    if (finders.size() < 3)
        return result; // 静默失败：换下一种二值化方式重试。
    const std::vector<GeometryHypothesis> hypotheses = enumerateHypotheses(finders);
    if (hypotheses.empty()) {
        result.error = QStringLiteral("图片中找到了类似定位图案，但无法构成有效的二维码。");
        return result;
    }
    QString lastError;
    for (const GeometryHypothesis& choice : hypotheses) {
        const FinderCandidate& a = finders[choice.a];
        const FinderCandidate& b = finders[choice.b];
        const FinderCandidate& c = finders[choice.c];
        Mapper mapper = affineFromTriple(a, b, c, choice.size);
        if (choice.version >= 2) {
            // 用左下角对齐图案做第四点，矫正缩放误差与轻微透视。
            QPointF alignmentModule;
            if (findAlignmentCenter(binary, mapper, choice.size, &alignmentModule)) {
                const QPointF module[4] = {QPointF(3.5, 3.5), QPointF(choice.size - 3.5, 3.5),
                    QPointF(3.5, choice.size - 3.5), alignmentModule};
                const QPointF pixel[4] = {QPointF(a.x, a.y), QPointF(b.x, b.y), QPointF(c.x, c.y),
                    mapper.map(alignmentModule.x(), alignmentModule.y())};
                Mapper refined;
                if (solveHomography(module, pixel, refined))
                    mapper = refined;
            }
        }
        ModuleGrid grid;
        grid.size = choice.size;
        grid.dark.assign(size_t(grid.size) * grid.size, 0);
        grid.isFunction.assign(size_t(grid.size) * grid.size, 0);
        if (!sampleModules(gray, otsu, binary, adaptive, inverted, mapper, grid)) {
            lastError = QStringLiteral("二维码区域超出图片边界，无法完整读取。");
            continue;
        }
        buildFunctionMap(choice.version, grid);
        const auto formats = validFormatValues();
        int bestMask = -1;
        int bestEcc = -1;
        int bestErrors = MaximumFormatErrors + 1;
        for (const int valid : formats) {
            for (const bool second : {false, true}) {
                const int errors = bitCount(readFormatCopy(grid, second) ^ valid);
                if (errors < bestErrors) {
                    bestErrors = errors;
                    const int data = (valid ^ 0x5412) >> 10;
                    bestMask = data & 7;   // 掩码编号位于数据位低 3 位。
                    bestEcc = (data >> 3) & 3; // 纠错级别（0=M、1=L、2=H、3=Q）。
                }
            }
        }
        if (bestMask < 0 || bestEcc < 0 || bestErrors > MaximumFormatErrors) {
            lastError = QStringLiteral("二维码格式信息损坏，无法确认掩码。");
            continue;
        }
        const std::vector<quint8> dataBits = collectDataBits(grid, bestMask);
        std::vector<quint8> codewords;
        codewords.reserve(dataBits.size() / 8);
        for (size_t index = 0; index + 7 < dataBits.size(); index += 8) {
            int value = 0;
            for (int i = 0; i < 8; ++i)
                value = value << 1 | dataBits[index + size_t(i)];
            codewords.push_back(quint8(value));
        }
        // 数据码字按块交织存放，先还原为逻辑顺序；结构不可用时退回原始顺序。
        std::vector<quint8> logical = deinterleaveCodewords(codewords, kBlockStructures[choice.version][bestEcc]);
        if (logical.empty())
            logical = codewords;
        QString text;
        QString error;
        if (!decodeSegments(logical, choice.version, &text, &error)) {
            lastError = error;
            continue;
        }
        result.text = text;
        result.version = choice.version;
        return result;
    }
    result.error = lastError;
    return result;
}

Result failure(Result result, const QString& message)
{
    result.error = message;
    return result;
}

}

Result decode(const QImage& image)
{
    if (image.isNull() || image.width() <= 0 || image.height() <= 0)
        return failure(Result(), QStringLiteral("图片为空，无法识别。"));
    const QImage gray = prepareGray(image);
    if (gray.isNull() || gray.width() < 21 || gray.height() < 21)
        return failure(Result(), QStringLiteral("图片太小，无法识别二维码。"));
    const int otsu = otsuThreshold(gray);
    Result last;
    const struct Attempt {
        bool adaptive;
        bool inverted;
    } attempts[] = {{false, false}, {true, false}, {false, true}, {true, true}};
    for (const Attempt& attempt : attempts) {
        Result attemptResult = decodeOnce(gray, otsu, attempt.adaptive, attempt.inverted);
        if (!attemptResult.text.isEmpty())
            return attemptResult;
        if (!attemptResult.error.isEmpty())
            last.error = attemptResult.error;
    }
    if (last.error.isEmpty())
        last.error = QStringLiteral("未在图片中识别到二维码。");
    return last;
}

}
