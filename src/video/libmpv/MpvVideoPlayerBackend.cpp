/*
 * Copyright 2010-2014 Bluecherry
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE. See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program. If not, see <http://www.gnu.org/licenses/>.
 */

#include <QString>
#include <QStringList>
#include <QProcess>
#include <QUrl>
#include <QRegExp>
#include <QByteArray>
#include <QTimer>
#include <QDebug>
//#include <stdlib.h>

#include "MpvVideoPlayerBackend.h"
#include "video/VideoHttpBuffer.h"
#include "mpv/qthelper.hpp"

#define DOWNLOADED_THRESHOLD 10

static void wakeup(void *ptr)
{
    MpvVideoPlayerBackend *backend = (MpvVideoPlayerBackend*) ptr;
    backend->emitEvents();
}

MpvVideoPlayerBackend::~MpvVideoPlayerBackend()
{
    clear();
}

MpvVideoPlayerBackend::MpvVideoPlayerBackend(QObject *parent)
    : VideoPlayerBackend(parent),
      m_videoBuffer(0), m_state(Stopped),
      m_playbackSpeed(1.0), m_mpv(0),
      m_duration(-1), m_position(-1),
      m_id(0), m_errorMessage(QString()),
      m_playDuringDownload(false), m_pausedBySlowDownload(false)
{
    std::setlocale(LC_NUMERIC, "C");

    qDebug() << "MpvVideoPlayerBackend() this =" << this << "\n";
}

void MpvVideoPlayerBackend::setVideoBuffer(VideoHttpBuffer *videoHttpBuffer)
{
    if (m_videoBuffer)
    {
        disconnect(m_videoBuffer, 0, this, 0);
        m_videoBuffer->clearPlayback();
        m_videoBuffer->deleteLater();
    }

    m_videoBuffer = videoHttpBuffer;

    if (m_videoBuffer)
    {
        connect(m_videoBuffer, SIGNAL(bufferingStarted()), this, SIGNAL(bufferingStarted()));
        connect(m_videoBuffer, SIGNAL(bufferingStopped()), this, SIGNAL(bufferingStopped()));
        connect(m_videoBuffer, SIGNAL(bufferingFinished()), SLOT(playIfReady()));
        connect(m_videoBuffer, SIGNAL(streamError(QString)), SLOT(streamError(QString)));
    }
}

void MpvVideoPlayerBackend::checkDownloadAndPlayProgress(double position)
{
    if (!m_playDuringDownload)
        return;

    if (m_videoBuffer->isBufferingFinished())
    {
        m_playDuringDownload = false;
        m_pausedBySlowDownload = false;
        //disconnect(this, SIGNAL(currentPosition(double)), this, SLOT(checkDownloadAndPlayProgress(double)));

        return;
    }

    if (m_playDuringDownload && m_state == Playing)
    {
        if (m_videoBuffer->bufferedPercent() - qRound((position / m_duration) * 100) < DOWNLOADED_THRESHOLD)
        {
            pause();
            m_pausedBySlowDownload = true;
            qDebug() << "paused because download progress is slower than playback progress";
        }
    }

    if (m_pausedBySlowDownload && m_state == Paused)
    {
        if (m_videoBuffer->bufferedPercent() - qRound((position / m_duration) * 100) > DOWNLOADED_THRESHOLD)
        {
            m_pausedBySlowDownload = false;
            play();
            qDebug() << "continued playback after downloading portion of file";
        }
    }
}

void MpvVideoPlayerBackend::playDuringDownloadTimerShot()
{
    if (m_videoBuffer->isBufferingFinished())
    {
        m_playDuringDownload = false;
        m_pausedBySlowDownload = false;
        return;
    }

    if (m_videoBuffer->bufferedPercent() > DOWNLOADED_THRESHOLD)
    {
        m_playDuringDownload = true;

        qDebug() << "started playback while download is in progress";
        //connect(this, SIGNAL(currentPosition(double)), this, SLOT(checkDownloadAndPlayProgress(double)));
        playIfReady();
    }
    else
        QTimer::singleShot(1000, this, SLOT(playDuringDownloadTimerShot()));
}

void MpvVideoPlayerBackend::setWindowId(quint64 wid)
{
    m_id = wid;
}

bool MpvVideoPlayerBackend::createMpvProcess()
{
    m_mpv = mpv_create();
    if (!m_mpv)
    {
        qDebug() << "MpvVideoPlayerBackend: Can't create mpv instance!\n";
        return false;
    }

    mpv_set_option(m_mpv, "wid", MPV_FORMAT_INT64, &m_id);

    mpv_set_option_string(m_mpv, "input-default-bindings", "yes");
    mpv_set_option_string(m_mpv, "input-vo-keyboard", "yes");

    mpv_observe_property(m_mpv, 0, "time-pos", MPV_FORMAT_DOUBLE);
    mpv_observe_property(m_mpv, 0, "duration", MPV_FORMAT_DOUBLE);

    mpv_request_log_messages(m_mpv, "fatal");

    connect(this, SIGNAL(mpvEvents()), this, SLOT(receiveMpvEvents()), Qt::QueuedConnection);
    mpv_set_wakeup_callback(m_mpv, wakeup, this);

    if (mpv_initialize(m_mpv) < 0)
    {
        qDebug() << "MpvVideoPlayerBackend: mpv failed to initialize!\n";
        return false;
    }

    return true;
}

bool MpvVideoPlayerBackend::start(const QUrl &url)
{
    qDebug() << "MpvVideoPlayerBackend::start url =" << url << "\n";
    if (state() == PermanentError)
        return false;

    if (!m_id)
    {
        setError(false, tr("Window id is not set"));
        return false;
    }

    if (m_mpv)
    {
        int pause = 1;
        mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);
        mpv_terminate_destroy(m_mpv);
        m_mpv = NULL;
    }

    if (!createMpvProcess())
    {
        //setError(false, tr("MpvVideoPlayerBackend: MPV failed to create!"));
        qDebug() << "MpvVideoPlayerBackend: start - failed to create MPV \n";
        return false;
    }

    /* Buffered HTTP source */
    setVideoBuffer(new VideoHttpBuffer(url));

    m_videoBuffer->startBuffering();

    QTimer::singleShot(1000, this, SLOT(playDuringDownloadTimerShot()));

    m_playbackSpeed = 1.0;
    qDebug() << "MpvVideoPlayerBackend:: MPV process started\n";
    return true;
}

void MpvVideoPlayerBackend::clear()
{
    if (m_mpv)
        mpv_terminate_destroy(m_mpv);

    setVideoBuffer(0);

    m_state = Stopped;
    m_errorMessage.clear();
}

void MpvVideoPlayerBackend::setError(bool permanent, const QString message)
{
    VideoState old = m_state;
    m_state = permanent ? PermanentError : Error;
    m_errorMessage = message;
    emit stateChanged(m_state, old);
}

void MpvVideoPlayerBackend::streamError(const QString &message)
{
    qDebug() << "MpvVideoPlayerBackend: stopping stream due to error:" << message;

    //close mplayer process?

    setError(true, message);
}

bool MpvVideoPlayerBackend::saveScreenshot(QString &file)
{
    /*if (!m_mplayer || !m_mplayer->isRunning() || !m_mplayer->isReadyToPlay())
        return false;

    return m_mplayer->saveScreenshot(file);*/

    return false;
}


void MpvVideoPlayerBackend::handleEof()
{
    qDebug() << "EOF\n";
    VideoState old = m_state;
    m_state = Done;
    emit stateChanged(m_state, old);
    emit endOfStream();
}

void MpvVideoPlayerBackend::mplayerReady()
{
    qDebug() << this << "mplayer is ready to play\n";

    emit streamsInitialized(true);

    VideoState old = m_state;
    m_state = Playing;
    emit stateChanged(m_state, old);

    setSpeed(m_playbackSpeed);
}

void MpvVideoPlayerBackend::durationIsKnown()
{
    emit durationChanged(duration());
}

bool MpvVideoPlayerBackend::isSeekable() const
{
    return true;
}

void MpvVideoPlayerBackend::playIfReady()
{
    if (m_pausedBySlowDownload)
    {
        play();
        return;
    }

    //if (!m_mplayer || m_mplayer->isRunning() || !m_mplayer->start(m_videoBuffer->bufferFilePath()))
        //return;

    if (!m_mpv || m_state == Playing)
        return;

    qDebug() << "MpvVideoPlayerBackend::playIfReady()___1 \n";

    const QByteArray filename = m_videoBuffer->bufferFilePath().toUtf8();
    const char *args[] = { "loadfile", filename.data(), NULL };
    mpv_command_async(m_mpv, 0, args);

    VideoState old = m_state;
    m_state = Playing;
    emit stateChanged(m_state, old);
}

void MpvVideoPlayerBackend::play()
{
    //if (!m_mplayer || !m_mplayer->isRunning() || !m_mplayer->isReadyToPlay())
      //  return;

    if (!m_mpv || m_state == Playing)
        return;

    if (m_pausedBySlowDownload)
        return;

    //m_mplayer->play();

    int pause = 0;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);

    emit playbackSpeedChanged(m_playbackSpeed);

    VideoState old = m_state;
    m_state = Playing;
    emit stateChanged(m_state, old);
}

void MpvVideoPlayerBackend::pause()
{
    //if (!m_mplayer || !m_mplayer->isRunning() || !m_mplayer->isReadyToPlay())
        //return;

    if (!m_mpv)
        return;

    m_pausedBySlowDownload = false;

    int pause = 1;
    mpv_set_property(m_mpv, "pause", MPV_FORMAT_FLAG, &pause);

    VideoState old = m_state;
    m_state = Paused;
    emit stateChanged(m_state, old);
}

void MpvVideoPlayerBackend::restart()
{
    qDebug() << "MpvVideoPlayerBackend::restart()___1 \n";

    if (!m_mpv)
        return;

    //m_mplayer->deleteLater();
    mpv_terminate_destroy(m_mpv);
    m_mpv = NULL;

    //createMpvProcess();
    if (createMpvProcess())
    {
        setError(false, tr("MpvVideoWidget: MPV failed to create!"));
        return;
    }

    qDebug() << "MpvVideoPlayerBackend::restart()___2 \n";

    //m_mplayer->start(m_videoBuffer->bufferFilePath());
    const QByteArray filename = m_videoBuffer->bufferFilePath().toUtf8();
    const char *args[] = { "loadfile", filename.data(), NULL };
    mpv_command_async(m_mpv, 0, args);

    //VideoState old = m_state;
    //m_state = Stopped;
    //emit stateChanged(m_state, old);
}

void MpvVideoPlayerBackend::mute(bool mute)
{
    if (!m_mpv)
        return;

    int mut = (mute ? 1 : 0);
    mpv_set_property(m_mpv, "mute", MPV_FORMAT_FLAG, &mut);
}

void MpvVideoPlayerBackend::setVolume(double volume)
{
    if (!m_mpv)
        return;

    double vol = volume * 100.0;
    mpv_set_property(m_mpv, "volume", MPV_FORMAT_DOUBLE, &vol);
}

int MpvVideoPlayerBackend::duration() const
{
    if (!m_mpv)
        return -1;

    return m_duration < 0 ? -1 : m_duration  * 1000.0;
}

int MpvVideoPlayerBackend::position() const
{
    if (!m_mpv)
        return -1;

    return m_position > 0 ? m_position * 1000.0 : -1;
}

void MpvVideoPlayerBackend::queryPosition() const
{
    /*if (!m_mplayer || !m_mplayer->isRunning())
    {
        return;
    }

    m_mplayer->queryPosition();*/
}

void MpvVideoPlayerBackend::setHardwareDecodingEnabled(bool enable)
{
    //implement later
}

bool MpvVideoPlayerBackend::seek(int position)
{
    if (!m_mpv)
        return false;

    if (m_playDuringDownload && (position/duration()*100) > m_videoBuffer->bufferedPercent())
        return false;

    char num[32];
    double pos = double (position);
    pos /= 1000;
    sprintf(num, "%.3f", pos);

    //qDebug() << "MpvVideoPlayerBackend::seek(int position)______ " << num << "\n";

    const char *cmd[] = { "seek", num, "absolute", NULL };
    mpv_command(m_mpv, cmd);

    return true;
}

bool MpvVideoPlayerBackend::setSpeed(double speed)
{
    if (!m_mpv)
        return false;

    if (speed == m_playbackSpeed)
        return true;

    mpv_set_property(m_mpv, "speed", MPV_FORMAT_DOUBLE, &speed);

    m_playbackSpeed = speed;
    m_lastspeed = speed;
    emit playbackSpeedChanged(m_playbackSpeed);

    return speed;
}

void MpvVideoPlayerBackend::emitEvents()
{
    emit mpvEvents();
}

void MpvVideoPlayerBackend::receiveMpvEvents()
{
    // This slot is invoked by wakeup() (through the mpv_events signal).
    while (m_mpv)
    {
        mpv_event *event = mpv_wait_event(m_mpv, 0);
        if (event->event_id == MPV_EVENT_NONE)
            break;
        handleMpvEvent(event);
    }
}

void MpvVideoPlayerBackend::handleMpvEvent(mpv_event *event)
{
    switch (event->event_id)
    {
    case MPV_EVENT_PROPERTY_CHANGE:
    {
        mpv_event_property *prop = (mpv_event_property *)event->data;
        if (strcmp(prop->name, "time-pos") == 0)
        {
            if (prop->format == MPV_FORMAT_DOUBLE)
            {
                m_position = *(double *)prop->data;
                emit currentPosition(m_position);
            }
        }
        else if (strcmp(prop->name, "duration") == 0)
        {
            if (prop->format == MPV_FORMAT_DOUBLE)
            {
                double duration = *(double *)prop->data;
                if (m_duration != duration)
                {
                    m_duration = duration;
                    durationIsKnown();
                }
            }
        }

        qDebug() << "pos= " << m_position << "    duration= " << m_duration;

        break;
    }
    case MPV_EVENT_VIDEO_RECONFIG:
    {
        // Retrieve the new video size.
        int64_t w, h;
        if (mpv_get_property(m_mpv, "dwidth", MPV_FORMAT_INT64, &w) >= 0 &&
            mpv_get_property(m_mpv, "dheight", MPV_FORMAT_INT64, &h) >= 0 &&
            w > 0 && h > 0)
        {
            // Note that the MPV_EVENT_VIDEO_RECONFIG event doesn't necessarily
            // imply a resize, and you should check yourself if the video
            // dimensions really changed.
            // mpv itself will scale/letter box the video to the container size
            // if the video doesn't fit.
            //std::stringstream ss;
            //ss << "Reconfig: " << w << " " << h;
            //statusBar()->showMessage(QString::fromStdString(ss.str()));
        }
        break;
    }
    case MPV_EVENT_LOG_MESSAGE:
    {
        struct mpv_event_log_message *msg = (struct mpv_event_log_message*) event->data;

        qDebug() << "MpvVideoWidget: [" << msg->prefix << "] "
                 << msg->level << ": " << msg->text << "\n";
        break;
    }
    case MPV_EVENT_SHUTDOWN:
    {
        mpv_terminate_destroy(m_mpv);
        m_mpv = NULL;
        break;
    }
    default: ;
        // Ignore uninteresting or unknown events.
    }
}

