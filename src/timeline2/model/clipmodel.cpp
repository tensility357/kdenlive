/***************************************************************************
 *   Copyright (C) 2017 by Nicolas Carion                                  *
 *   This file is part of Kdenlive. See www.kdenlive.org.                  *
 *                                                                         *
 *   This program is free software; you can redistribute it and/or modify  *
 *   it under the terms of the GNU General Public License as published by  *
 *   the Free Software Foundation; either version 2 of the License, or     *
 *   (at your option) version 3 or any later version accepted by the       *
 *   membership of KDE e.V. (or its successor approved  by the membership  *
 *   of KDE e.V.), which shall act as a proxy defined in Section 14 of     *
 *   version 3 of the license.                                             *
 *                                                                         *
 *   This program is distributed in the hope that it will be useful,       *
 *   but WITHOUT ANY WARRANTY; without even the implied warranty of        *
 *   MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the         *
 *   GNU General Public License for more details.                          *
 *                                                                         *
 *   You should have received a copy of the GNU General Public License     *
 *   along with this program.  If not, see <http://www.gnu.org/licenses/>. *
 ***************************************************************************/
#include "clipmodel.hpp"
#include "bin/projectitemmodel.h"
#include "bin/projectclip.h"
#include "core.h"
#include "effects/effectstack/model/effectstackmodel.hpp"
#include "macros.hpp"
#include "timelinemodel.hpp"
#include "trackmodel.hpp"
#include <QDebug>
#include <mlt++/MltProducer.h>
#include <utility>

// this can be deleted
#include "bin/model/markerlistmodel.hpp"
#include "gentime.h"
#include <effects/effectsrepository.hpp>

ClipModel::ClipModel(std::shared_ptr<TimelineModel> parent, std::shared_ptr<Mlt::Producer> prod, const QString &binClipId, int id)
    : MoveableItem<Mlt::Producer>(parent, id)
    , m_producer(std::move(prod))
    , m_effectStack(EffectStackModel::construct(m_producer, {ObjectType::TimelineClip, m_id}, parent->m_undoStack))
    , m_binClipId(binClipId)
{
    m_producer->set("kdenlive:id", binClipId.toUtf8().constData());
    m_producer->set("_kdenlive_cid", getId());
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    if (binClip) {
        m_endlessResize = !binClip->hasLimitedDuration();
    } else {
        m_endlessResize = false;
    }
}

int ClipModel::construct(const std::shared_ptr<TimelineModel> &parent, const QString &binClipId, int id)
{
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(binClipId);
    std::shared_ptr<Mlt::Producer> originalProducer = binClip->originalProducer();
    std::shared_ptr<Mlt::Producer> cutProducer(originalProducer->cut());
    return construct(parent, binClipId, cutProducer, id);
}

int ClipModel::construct(const std::shared_ptr<TimelineModel> &parent, const QString &binClipId, std::shared_ptr<Mlt::Producer> producer, int id)
{
    std::shared_ptr<ClipModel> clip(new ClipModel(parent, producer, binClipId, id));
    id = clip->m_id;
    parent->registerClip(clip);
    clip->m_effectStack->loadEffects();
    return id;
}

void ClipModel::registerClipToBin()
{
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    if (!binClip) {
        qDebug() << "Error : Bin clip for id: " << m_binClipId << " NOT AVAILABLE!!!";
    }
    qDebug() << "REGISTRATION " << m_id << "ptr count"<<m_parent.use_count();
    binClip->registerTimelineClip(m_parent, m_id);
}

void ClipModel::deregisterClipToBin()
{
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    binClip->deregisterTimelineClip(m_id);
}

ClipModel::~ClipModel()
{
}

bool ClipModel::requestResize(int size, bool right, Fun &undo, Fun &redo)
{
    QWriteLocker locker(&m_lock);
    qDebug() << "RESIZE CLIP" << m_id << "target size="<<size<<"right="<<right<<"endless="<<m_endlessResize<<"length"<<m_producer->get_length();
    if (!m_endlessResize && (size <= 0 || size > m_producer->get_length())) {
        return false;
    }
    int delta = getPlaytime() - size;
    int in = m_producer->get_in();
    int out = m_producer->get_out();
    int old_in = in, old_out = out;
    // check if there is enough space on the chosen side
    if (!right && in + delta < 0 && !m_endlessResize) {
        return false;
    }
    if (!m_endlessResize && right && out - delta >= m_producer->get_length()) {
        return false;
    }
    if (right) {
        out -= delta;
    } else {
        in += delta;
    }
    qDebug() << "Resize facts delta ="<<delta<<"old in"<<old_in<<"old_out"<<old_out<<"in"<<in<<"out"<<out;
    std::function<bool(void)> track_operation = []() { return true; };
    std::function<bool(void)> track_reverse = []() { return true; };
    if (m_currentTrackId != -1) {
        if (auto ptr = m_parent.lock()) {
            track_operation = ptr->getTrackById(m_currentTrackId)->requestClipResize_lambda(m_id, in, out, right);
        } else {
            qDebug() << "Error : Moving clip failed because parent timeline is not available anymore";
            Q_ASSERT(false);
        }
    }
    Fun operation = [this, in, out, track_operation]() {
        if (track_operation()) {
            int outPoint = in < 0 ? out - in : out;
            if (outPoint >= m_producer->get_length()) {
                m_producer->parent().set("length", outPoint + 1);
                m_producer->set("length", outPoint + 1);
            }
            m_producer->set_in_and_out(in < 0 ? 0 : in, outPoint);
            return true;
        }
        return false;
    };
    if (operation()) {
        // Now, we are in the state in which the timeline should be when we try to revert current action. So we can build the reverse action from here
        auto ptr = m_parent.lock();
        if (m_currentTrackId != -1 && ptr) {
            track_reverse = ptr->getTrackById(m_currentTrackId)->requestClipResize_lambda(m_id, old_in, old_out, right);
        }
        Fun reverse = [this, old_in, old_out, track_reverse]() {
            if (track_reverse()) {
                m_producer->set_in_and_out(old_in, old_out);
                return true;
            }
            return false;
        };
        UPDATE_UNDO_REDO(operation, reverse, undo, redo);
        return true;
    }
    return false;
}

const QString ClipModel::getProperty(const QString &name) const
{
    READ_LOCK();
    if (service()->parent().is_valid()) {
        return QString::fromUtf8(service()->parent().get(name.toUtf8().constData()));
    }
    return QString::fromUtf8(service()->get(name.toUtf8().constData()));
}

int ClipModel::getIntProperty(const QString &name) const
{
    READ_LOCK();
    if (service()->parent().is_valid()) {
        return service()->parent().get_int(name.toUtf8().constData());
    }
    return service()->get_int(name.toUtf8().constData());
}

double ClipModel::getDoubleProperty(const QString &name) const
{
    READ_LOCK();
    if (service()->parent().is_valid()) {
        return service()->parent().get_double(name.toUtf8().constData());
    }
    return service()->get_double(name.toUtf8().constData());
}

Mlt::Producer *ClipModel::service() const
{
    READ_LOCK();
    return m_producer.get();
}

int ClipModel::getPlaytime() const
{
    READ_LOCK();
    return m_producer->get_playtime();
}

void ClipModel::setTimelineEffectsEnabled(bool enabled)
{
    READ_LOCK();
    m_effectStack->setEffectStackEnabled(enabled);
}

bool ClipModel::addEffect(const QString &effectId)
{
    READ_LOCK();
    m_effectStack->appendEffect(effectId);
    return true;
}

bool ClipModel::copyEffect(std::shared_ptr<EffectStackModel> stackModel, int rowId)
{
    READ_LOCK();
    m_effectStack->copyEffect(stackModel->getEffectStackRow(rowId));
    return true;
}

bool ClipModel::removeFade(bool fromStart)
{
    READ_LOCK();
    m_effectStack->removeFade(fromStart);
    return true;
}

bool ClipModel::adjustEffectLength(const QString &effectName, int duration)
{
    READ_LOCK();
    m_effectStack->adjustFadeLength(duration, effectName == QLatin1String("fadein"), hasAudio(), !isAudioOnly());
    return true;
}

bool ClipModel::hasAudio() const
{
    READ_LOCK();
    QString service = getProperty("mlt_service");
    return service.contains(QStringLiteral("avformat")) && (getIntProperty(QStringLiteral("audio_index")) > -1);
}

bool ClipModel::isAudioOnly() const
{
    READ_LOCK();
    QString service = getProperty("mlt_service");
    return service.contains(QStringLiteral("avformat")) && (getIntProperty(QStringLiteral("video_index")) == -1);
}

void ClipModel::refreshProducerFromBin()
{
    if (getProperty("mlt_service") == QLatin1String("timewarp")) {
        // slowmotion producer, keep it
        int space = -1;
        if (m_currentTrackId != -1) {
            if (auto ptr = m_parent.lock()) {
                space = ptr->getTrackById(m_currentTrackId)->getBlankSizeNearClip(m_id, true);
            } else {
                qDebug() << "Error : Moving clip failed because parent timeline is not available anymore";
                Q_ASSERT(false);
            }
        }
        return useTimewarpProducer(m_producer->get_double("warp_speed"), space);
    }
    QWriteLocker locker(&m_lock);
    int in = getIn();
    int out = getOut();
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    std::shared_ptr<Mlt::Producer> originalProducer = binClip->originalProducer();
    m_producer.reset(originalProducer->cut(in, out));
    // replant effect stack in updated service
    m_effectStack->resetService(m_producer);
    m_producer->set("kdenlive:id", binClip->AbstractProjectItem::clipId().toUtf8().constData());
    m_producer->set("_kdenlive_cid", m_id);
    m_endlessResize = !binClip->hasLimitedDuration();
}

void ClipModel::useTimewarpProducer(double speed, int extraSpace)
{
    QWriteLocker locker(&m_lock);
    // TODO: disable timewarp on color clips
    int in = getIn();
    int out = getOut();
    int warp_in;
    int warp_out;
    double currentSpeed = 1.0;
    qDebug()<<"// SLOWMO CLIP SERVICE: "<<getProperty("mlt_service");
    if (getProperty("mlt_service") == QLatin1String("timewarp")) {
        // slowmotion producer, get current speed
        warp_in = m_producer->get_int("warp_in");
        warp_out = m_producer->get_int("warp_out");
        currentSpeed = m_producer->get_double("warp_speed");
    } else {
        // store original in/out
        warp_in = in;
        warp_out = out;
    }
    qDebug()<<"++++\n//// USING TIMEWARP: "<<warp_in<<"-"<<warp_out;
    in = warp_in / speed;
    out = warp_out / speed;
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    std::shared_ptr<Mlt::Producer> originalProducer = binClip->originalProducer();
    if (speed == 1.0) {
        m_producer.reset(originalProducer->cut(in, out));
    } else {
        QString resource = QString("%1:%2").arg(speed).arg(originalProducer->get("resource"));
        std::shared_ptr<Mlt::Producer> warpProducer(new Mlt::Producer(*originalProducer->profile(), "timewarp", resource.toUtf8().constData()));
        m_producer.reset(warpProducer->cut(in, out));
    }
    // replant effect stack in updated service
    m_effectStack->resetService(m_producer);
    m_producer->set("kdenlive:id", binClip->AbstractProjectItem::clipId().toUtf8().constData());
    m_producer->set("_kdenlive_cid", m_id);
    m_producer->set("warp_in", warp_in);
    m_producer->set("warp_out", warp_out);
    m_endlessResize = !binClip->hasLimitedDuration();
}

QVariant ClipModel::getAudioWaveform()
{
    READ_LOCK();
    std::shared_ptr<ProjectClip> binClip = pCore->projectItemModel()->getClipByBinID(m_binClipId);
    if (binClip) {
        return QVariant::fromValue(binClip->audioFrameCache);
    }
    return QVariant();
}

const QString &ClipModel::binId() const
{
    return m_binClipId;
}

std::shared_ptr<MarkerListModel> ClipModel::getMarkerModel() const
{
    READ_LOCK();
    return pCore->projectItemModel()->getClipByBinID(m_binClipId)->getMarkerModel();
}

int ClipModel::fadeIn() const
{
    return m_effectStack->getFadePosition(true);
}

int ClipModel::fadeOut() const
{
    return m_effectStack->getFadePosition(false);
}

double ClipModel::getSpeed() const
{
    if (getProperty("mlt_service") == QLatin1String("timewarp")) {
        // slowmotion producer, get current speed
        return m_producer->parent().get_double("warp_speed");
    }
    return 1.0;
}