#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// Practice Buffer: if you press jump a few frames before you can use it
// (landing, touching an orb), the press is remembered and applied the moment
// it becomes usable, as long as that happens inside the buffer window.
//
// v1.1: the buffer check now runs from update() (guaranteed hook), and the
// indicator shows how many presses were stored and how many were used.

namespace {
	bool enabled()      { return Mod::get()->getSettingValue<bool>("enabled"); }
	bool practiceOnly() { return Mod::get()->getSettingValue<bool>("practice-only"); }
	bool bufferOrbs()   { return Mod::get()->getSettingValue<bool>("buffer-orbs"); }
	bool bufferGround() { return Mod::get()->getSettingValue<bool>("buffer-ground"); }

	double windowSeconds() {
		return static_cast<double>(Mod::get()->getSettingValue<int64_t>("buffer-frames")) / 60.0;
	}
	double orbWindowSeconds() {
		return static_cast<double>(Mod::get()->getSettingValue<int64_t>("orb-frames")) / 60.0;
	}

	// Cube, ball, robot and spider jump from the ground. Ship, UFO, wave and swing don't.
	bool groundBufferable(PlayerObject* p) {
		return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
	}

	bool bufferActive(PlayLayer* pl) {
		if (!pl || !enabled()) return false;
		if (practiceOnly() && !pl->m_isPracticeMode) return false;
		return true;
	}

	constexpr char const* INDICATOR_ID = "buffer-indicator";

	// Debug counters shown on the indicator.
	int g_stored = 0;   // presses remembered while in the air
	int g_used = 0;     // remembered presses that turned into a jump/orb hit
}

// ---------- On-screen indicator ----------

class $modify(BufferPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		g_stored = 0;
		g_used = 0;

		auto label = CCLabelBMFont::create("Buffer ON", "bigFont.fnt");
		label->setID(INDICATOR_ID);
		label->setScale(0.35f);
		label->setOpacity(180);
		label->setColor({ 255, 200, 80 });
		label->setAnchorPoint({ 0.f, 0.f });
		label->setPosition({ 5.f, 5.f });
		label->setZOrder(1000);
		label->setVisible(bufferActive(this));
		this->addChild(label);
		return true;
	}
};

// ---------- The buffer itself ----------

class $modify(BufferGameLayer, GJBaseGameLayer) {
	struct Pending {
		bool active = false;        // a press is waiting to be used
		double time = 0.0;          // level time of the press
		bool realHeld = false;      // is the physical button held right now?
		bool injectedHeld = false;  // did we send a press that still needs releasing?
		double injectTime = 0.0;
	};

	struct Fields {
		Pending p[2];
		int lastStored = -1;
		int lastUsed = -1;
		bool lastVisible = false;
	};

	bool isActivePlay() {
		auto pl = PlayLayer::get();
		return pl && static_cast<GJBaseGameLayer*>(pl) == this && bufferActive(pl);
	}

	void handleButton(bool down, int button, bool isPlayer1) {
		if (button != 1 || !this->isActivePlay()) {
			GJBaseGameLayer::handleButton(down, button, isPlayer1);
			return;
		}

		auto& p = m_fields->p[isPlayer1 ? 0 : 1];
		auto player = isPlayer1 ? m_player1 : m_player2;

		if (down) {
			p.realHeld = true;
			bool usableNow = player && (player->m_isOnGround || !player->m_touchedRings.empty());
			bool store = player && !player->m_isDead && !usableNow;
			p.active = store;
			p.time = m_gameState.m_levelTime;
			if (store) {
				g_stored++;
				log::debug("stored press at t={:.3f}", p.time);
			}
		} else {
			p.realHeld = false;
			p.injectedHeld = false;
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void update(float dt) {
		if (this->isActivePlay()) {
			this->bufferStep();
		}
		GJBaseGameLayer::update(dt);
		this->updateIndicator();
	}

	void bufferStep() {
		double now = m_gameState.m_levelTime;
		double window = windowSeconds();

		for (int i = 0; i < 2; ++i) {
			bool isP1 = (i == 0);
			auto player = isP1 ? m_player1 : m_player2;
			if (!player) continue;
			auto& p = m_fields->p[i];

			// Release a press we sent once the real button is up and time has moved on.
			if (p.injectedHeld && !p.realHeld && now > p.injectTime) {
				GJBaseGameLayer::handleButton(false, 1, isP1);
				p.injectedHeld = false;
			}

			if (!p.active) continue;

			// now < p.time: restarted or respawned at a checkpoint.
			if (player->m_isDead || now < p.time || now - p.time > window) {
				if (now - p.time > window) log::debug("press expired after {:.3f}s", now - p.time);
				p.active = false;
				continue;
			}

			// Orbs use their own, shorter window: an early tap only counts for an orb
			// you reach almost immediately, not one you were planning to fly past.
			double age = now - p.time;
			bool onRing   = bufferOrbs()   && !player->m_touchedRings.empty() && age <= orbWindowSeconds();
			bool onGround = bufferGround() && player->m_isOnGround && groundBufferable(player);

			if (onRing || onGround) {
				if (onRing || !p.realHeld) {
					GJBaseGameLayer::handleButton(true, 1, isP1);
					p.injectedHeld = true;
					p.injectTime = now;
				}
				g_used++;
				log::debug("used press after {:.3f}s ({})", now - p.time, onRing ? "orb" : "ground");
				p.active = false;
			}
		}
	}

	void updateIndicator() {
		auto pl = PlayLayer::get();
		if (!pl || static_cast<GJBaseGameLayer*>(pl) != this) return;
		auto label = typeinfo_cast<CCLabelBMFont*>(pl->getChildByID(INDICATOR_ID));
		if (!label) return;

		bool visible = bufferActive(pl);
		if (visible != m_fields->lastVisible) {
			label->setVisible(visible);
			m_fields->lastVisible = visible;
		}
		if (visible && (g_stored != m_fields->lastStored || g_used != m_fields->lastUsed)) {
			label->setString(fmt::format("Buffer ON  lagret: {}  brukt: {}", g_stored, g_used).c_str());
			m_fields->lastStored = g_stored;
			m_fields->lastUsed = g_used;
		}
	}
};
