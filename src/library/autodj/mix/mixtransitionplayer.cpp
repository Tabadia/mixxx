#include "library/autodj/mix/mixtransitionplayer.h"

#include "control/controlproxy.h"
#include "mixer/playerinfo.h"
#include "moc_mixtransitionplayer.cpp"
#include "track/track.h"
#include "util/logger.h"

namespace {

const mixxx::Logger kLogger("MixTransitionPlayer");

const QString kDeckGroups[2] = {QStringLiteral("[Channel1]"), QStringLiteral("[Channel2]")};
constexpr int kTickMs = 20;
// Only fire when the trigger was crossed during normal playback, not after
// a seek far past it.
constexpr double kMaxCrossingJumpSeconds = 0.75;

} // anonymous namespace

MixTransitionPlayer::MixTransitionPlayer(MixTransitionStore* pStore, QObject* pParent)
        : QObject(pParent),
          m_pStore(pStore),
          m_running(false),
          m_from(0) {
    for (int i = 0; i < 2; ++i) {
        const auto flags = ControlFlag::AllowMissingOrInvalid;
        m_decks[i].pPlay = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("play"), nullptr, flags);
        m_decks[i].pPlayPos = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("playposition"), nullptr, flags);
        m_decks[i].pDuration = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("duration"), nullptr, flags);
        m_decks[i].pTrackLoaded = std::make_unique<ControlProxy>(
                kDeckGroups[i], QStringLiteral("track_loaded"), nullptr, flags);
    }
    m_pAutoDJEnabled = std::make_unique<ControlProxy>(QStringLiteral("[AutoDJ]"),
            QStringLiteral("enabled"),
            nullptr,
            ControlFlag::AllowMissingOrInvalid);
    m_pCrossfader = std::make_unique<ControlProxy>(QStringLiteral("[Master]"),
            QStringLiteral("crossfader"),
            nullptr,
            ControlFlag::AllowMissingOrInvalid);
    m_timer.setInterval(kTickMs);
    connect(&m_timer, &QTimer::timeout, this, &MixTransitionPlayer::slotTick);
    m_timer.start();
}

MixTransitionPlayer::~MixTransitionPlayer() {
    m_driver.finish();
}

double MixTransitionPlayer::seconds(int deck) const {
    return m_decks[deck].pPlayPos->get() * m_decks[deck].pDuration->get();
}

bool MixTransitionPlayer::isPlaying(int deck) const {
    return m_decks[deck].pPlay->get() > 0;
}

void MixTransitionPlayer::slotTick() {
    const bool autoDJ = m_pAutoDJEnabled->valid() && m_pAutoDJEnabled->get() > 0;

    if (m_running) {
        const int to = 1 - m_from;
        if (autoDJ || !isPlaying(m_from) || !isPlaying(to)) {
            // Interrupted (stopped by hand or Auto DJ took over): finish
            // cleanly so the EQs and faders are restored.
            stop();
            return;
        }
        const double length = m_transition.length;
        const double progress = length > 0
                ? (seconds(m_from) - m_transition.fromTrigger) / length
                : 1.0;
        if (progress >= 1.0) {
            m_decks[m_from].pPlay->set(0.0);
            emit progressChanged(m_from, 1.0);
            stop();
            return;
        }
        m_driver.apply(progress);
        emit progressChanged(m_from, progress);
        return;
    }

    for (int deck = 0; deck < 2; ++deck) {
        const double now = seconds(deck);
        const double last = m_decks[deck].lastSeconds;
        m_decks[deck].lastSeconds = isPlaying(deck) ? now : -1.0;
        if (autoDJ || !m_pStore->isEnabled() || !isPlaying(deck) || last < 0) {
            continue;
        }
        const int other = 1 - deck;
        if (isPlaying(other) || m_decks[other].pTrackLoaded->get() <= 0) {
            continue;
        }
        const TrackPointer pFrom = PlayerInfo::instance().getTrackInfo(kDeckGroups[deck]);
        const TrackPointer pTo = PlayerInfo::instance().getTrackInfo(kDeckGroups[other]);
        if (!pFrom || !pTo) {
            continue;
        }
        const std::optional<MixTransition> transition =
                m_pStore->get(pFrom->getId(), pTo->getId());
        if (!transition) {
            continue;
        }
        const double trigger = transition->fromTrigger;
        if (last < trigger && now >= trigger && now - last <= kMaxCrossingJumpSeconds) {
            start(deck, *transition, pTo);
            return;
        }
    }
}

void MixTransitionPlayer::start(int from, const MixTransition& transition, const TrackPointer& pTo) {
    const int to = 1 - from;
    m_transition = transition;
    m_from = from;
    const double toDuration = m_decks[to].pDuration->get();
    if (toDuration > 0) {
        m_decks[to].pPlayPos->set(transition.toStart / toDuration);
    }
    // Faders do the fading: center the crossfader so both decks are heard
    // (with the default crossfader curve both are at full level there).
    m_pCrossfader->set(0.0);
    m_driver.begin(kDeckGroups[from], kDeckGroups[to], transition, pTo ? pTo->getBpm() : 0.0);
    m_decks[to].pPlay->set(1.0);
    m_driver.started();
    m_running = true;
    kLogger.info() << "Transition" << kDeckGroups[from] << "->" << kDeckGroups[to]
                   << "at" << transition.fromTrigger << "s for" << transition.length << "s";
    emit runningChanged(true);
}

void MixTransitionPlayer::stop() {
    m_driver.finish();
    m_running = false;
    for (Deck& deck : m_decks) {
        deck.lastSeconds = -1.0;
    }
    emit runningChanged(false);
}
