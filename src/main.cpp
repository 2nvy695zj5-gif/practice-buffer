#include <Geode/Geode.hpp>
#include <Geode/modify/GJBaseGameLayer.hpp>
#include <Geode/modify/PlayLayer.hpp>

using namespace geode::prelude;

// Practice Buffer: if you press jump a few frames before you can use it
// (landing, touching an orb), the press is remembered and applied the moment
// it becomes usable, as long as that happens inside the buffer window.
//
// Safety: by default it only runs in practice mode, and an on-screen label
// shows whenever the buffer is active.

namespace {
	bool enabled()      { return Mod::get()->getSettingValue<bool>("enabled"); }
	bool practiceOnly() { return Mod::get()->getSettingValue<bool>("practice-only"); }
	bool bufferOrbs()   { return Mod::get()->getSettingValue<bool>("buffer-orbs"); }
	bool bufferGround() { return Mod::get()->getSettingValue<bool>("buffer-ground"); }

	double windowSeconds() {
		return static_cast<double>(Mod::get()->getSettingValue<int64_t>("buffer-frames")) / 60.0;
	}

	// Cube, ball, robot and spider jump from the ground. Ship, UFO, wave and swing don't.
	bool groundBufferable(PlayerObject* p) {
		return !(p->m_isShip || p->m_isBird || p->m_isDart || p->m_isSwing);
	}

	// Is the buffer allowed to act in this PlayLayer right now?
	bool bufferActive(PlayLayer* pl) {
		if (!pl || !enabled()) return false;
		if (practiceOnly() && !pl->m_isPracticeMode) return false;
		return true;
	}

	constexpr char const* INDICATOR_ID = "buffer-indicator";
}

// ---------- On-screen indicator ----------

class $modify(BufferPlayLayer, PlayLayer) {
	bool init(GJGameLevel* level, bool useReplay, bool dontCreateObjects) {
		if (!PlayLayer::init(level, useReplay, dontCreateObjects)) return false;

		auto label = CCLabelBMFont::create("Buffer ON", "bigFont.fnt");
		label->setID(INDICATOR_ID);
		label->setScale(0.35f);
		label->setOpacity(160);
		label->setColor({ 255, 200, 80 });
		label->setAnchorPoint({ 0.f, 0.f });
		label->setPosition({ 5.f, 5.f });
		label->setZOrder(1000);
		label->setVisible(bufferActive(this));
		this->addChild(label);
		return true;
	}

	void postUpdate(float dt) {
		PlayLayer::postUpdate(dt);
		if (auto label = this->getChildByID(INDICATOR_ID)) {
			label->setVisible(bufferActive(this));
		}
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
			// Can't be used right now? Remember it.
			bool usableNow = player && (player->m_isOnGround || !player->m_touchedRings.empty());
			p.active = player && !player->m_isDead && !usableNow;
			p.time = m_gameState.m_levelTime;
		} else {
			p.realHeld = false;
			p.injectedHeld = false;
		}

		GJBaseGameLayer::handleButton(down, button, isPlayer1);
	}

	void processCommands(float dt, bool isHalfTick, bool isLastTick) {
		if (this->isActivePlay()) {
			this->bufferStep();
		}
		GJBaseGameLayer::processCommands(dt, isHalfTick, isLastTick);
	}

	void bufferStep() {
		double now = m_gameState.m_levelTime;
		double window = windowSeconds();

		for (int i = 0; i < 2; ++i) {
			bool isP1 = (i == 0);
			auto player = isP1 ? m_player1 : m_player2;
			if (!player) continue;
			auto& p = m_fields->p[i];

			// Release a press we sent once the real button is no longer held.
			if (p.injectedHeld && !p.realHeld && now > p.injectTime) {
				GJBaseGameLayer::handleButton(false, 1, isP1);
				p.injectedHeld = false;
			}

			if (!p.active) continue;

			// now < p.time: level restarted or respawned at a checkpoint.
			if (player->m_isDead || now < p.time || now - p.time > window) {
				p.active = false;
				continue;
			}

			bool onRing   = bufferOrbs()   && !player->m_touchedRings.empty();
			bool onGround = bufferGround() && player->m_isOnGround && groundBufferable(player);

			if (onRing || onGround) {
				// Holding on landing: the game already jumps by itself.
				// For orbs (or if you let go) we send the press.
				if (onRing || !p.realHeld) {
					GJBaseGameLayer::handleButton(true, 1, isP1);
					p.injectedHeld = true;
					p.injectTime = now;
				}
				p.active = false;
			}
		}
	}
};
