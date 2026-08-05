#include "ObsbotVideoNode.h"

#include <QDir>
#include <QStringList>

#include <algorithm>
#include <cerrno>

#include <fcntl.h>
#include <linux/videodev2.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace {

// ioctl with EINTR retry (signals can interrupt V4L2 calls).
int xioctl(int fd, unsigned long req, void *arg) {
    int r;
    do { r = ::ioctl(fd, req, arg); } while (r == -1 && errno == EINTR);
    return r;
}

} // namespace

QString ObsbotVideoNode::find() {
    QStringList nodes = QDir(QStringLiteral("/dev"))
                            .entryList({QStringLiteral("video*")}, QDir::System | QDir::Files);
    // Numeric order (lexical puts video10 before video2) so the pick is stable
    // across boots when several nodes exist.
    std::sort(nodes.begin(), nodes.end(), [](const QString &a, const QString &b) {
        return a.mid(5).toInt() < b.mid(5).toInt();
    });
    for (const QString &n : nodes) {
        const QString path = QStringLiteral("/dev/") + n;
        const int fd = ::open(path.toLocal8Bit().constData(), O_RDWR | O_NONBLOCK | O_CLOEXEC);
        if (fd < 0) continue;
        v4l2_capability cap{};
        const bool ok = (xioctl(fd, VIDIOC_QUERYCAP, &cap) == 0);
        ::close(fd);
        if (!ok) continue;
        // Metadata nodes report the SAME card name as the capture node (the
        // Tiny 3 presents two), so the capability bit is what separates them.
        const __u32 caps = (cap.capabilities & V4L2_CAP_DEVICE_CAPS) ? cap.device_caps
                                                                     : cap.capabilities;
        if (!(caps & V4L2_CAP_VIDEO_CAPTURE)) continue;
        const QString card = QString::fromLatin1(reinterpret_cast<const char *>(cap.card));
        if (!card.contains(QLatin1String("OBSBOT"), Qt::CaseInsensitive)) continue;
        return path;
    }
    return {};
}
