#include <IrrlichtDevice.h>
#include <IGUIWindow.h>
#include <IGUIStaticText.h>
#include "replay_mode.h"
#include "duelclient.h"
#include "game.h"
#include "single_mode.h"
#include "sound_manager.h"
#include "image_downloader.h"
#include <set>
#include <chrono>
#include <thread>
#include <cstdlib>

namespace ygo {

DuelPtr ReplayMode::pduel = nullptr;
bool ReplayMode::yrp = false;
Replay ReplayMode::cur_replay{};
Replay* ReplayMode::cur_yrp = nullptr;
bool ReplayMode::is_continuing = true;
bool ReplayMode::is_closing = false;
bool ReplayMode::is_pausing = false;
bool ReplayMode::is_paused = false;
bool ReplayMode::is_swapping = false;
bool ReplayMode::is_restarting = false;
bool ReplayMode::exit_pending = false;
int ReplayMode::skip_turn = 0;
int ReplayMode::current_step = 0;
int ReplayMode::skip_step = 0;
epro::thread ReplayMode::replay_thread;

void ReplayMode::CollectReplayCardCodes(std::set<uint32_t>& card_codes) {
	// Determine which replay to use
	Replay* replay_to_scan = yrp ? cur_yrp : &cur_replay;
	if(!replay_to_scan)
		return;
	
	// Collect cards from decks
	const auto& decks = replay_to_scan->GetPlayerDecks();
	for(const auto& deck : decks) {
		for(uint32_t code : deck.main_deck) {
			if(code != 0)
				card_codes.insert(code);
		}
		for(uint32_t code : deck.extra_deck) {
			if(code != 0)
				card_codes.insert(code);
		}
	}
	
	// Collect cards from rule cards
	const auto& rule_cards = replay_to_scan->GetRuleCards();
	for(uint32_t code : rule_cards) {
		if(code != 0)
			card_codes.insert(code);
	}
	
	// Collect cards from replay packets (only for new replay format)
	if(!yrp) {
		const auto& packets = cur_replay.packets_stream;
		for(const auto& packet : packets) {
			const uint8_t* pbuf = packet.data();
			switch(packet.message) {
				case MSG_UPDATE_CARD: {
					// Skip player, location, sequence
					pbuf += 3;
					// Read card data - the card code is at the beginning of the data
					if(packet.buff_size() > 7) {
						uint32_t flag = BufferIO::Read<uint32_t>(pbuf);
						if(flag & QUERY_CODE) {
							uint32_t code = BufferIO::Read<uint32_t>(pbuf);
							if(code != 0)
								card_codes.insert(code);
						}
					}
					break;
				}
				case MSG_MOVE: {
					uint32_t code = BufferIO::Read<uint32_t>(pbuf);
					if(code != 0)
						card_codes.insert(code);
					break;
				}
				case MSG_UPDATE_DATA: {
					// Skip player and location
					pbuf += 2;
					// Read card data
					if(packet.buff_size() > 6) {
						uint32_t flag = BufferIO::Read<uint32_t>(pbuf);
						if(flag & QUERY_CODE) {
							uint32_t code = BufferIO::Read<uint32_t>(pbuf);
							if(code != 0)
								card_codes.insert(code);
						}
					}
					break;
				}
				case MSG_DRAW:
				case MSG_CONFIRM_CARDS:
				case MSG_SHUFFLE_DECK:
				case MSG_SHUFFLE_HAND:
				case MSG_SHUFFLE_EXTRA:
				case MSG_CONFIRM_DECKTOP:
				case MSG_CONFIRM_EXTRATOP: {
					// These messages may contain card codes, but parsing them correctly
					// requires more complex logic. For now, we rely on MSG_UPDATE_CARD
					// and MSG_MOVE which should capture all cards that appear in the replay.
					break;
				}
			}
		}
	}
}

void ReplayMode::DownloadReplayImages(const std::set<uint32_t>& card_codes) {
	if(!gImageDownloader || card_codes.empty())
		return;
	
	// Queue all cards for download
	for(uint32_t code : card_codes) {
		gImageDownloader->AddToDownloadQueue(code, imgType::ART);
	}
	
	// Wait for downloads to complete or timeout (30 seconds max)
	const int max_wait_time_ms = 30000;
	const int check_interval_ms = 100;
	int elapsed_time = 0;
	
	while(elapsed_time < max_wait_time_ms) {
		bool all_done = true;
		int downloading_count = 0;
		
		for(uint32_t code : card_codes) {
			auto status = gImageDownloader->GetDownloadStatus(code, imgType::ART);
			if(status == ImageDownloader::downloadStatus::DOWNLOADING) {
				all_done = false;
				downloading_count++;
			} else if(status == ImageDownloader::downloadStatus::NONE) {
				all_done = false;
			}
		}
		
		if(all_done)
			break;
		
		std::this_thread::sleep_for(std::chrono::milliseconds(check_interval_ms));
		elapsed_time += check_interval_ms;
	}
}

bool ReplayMode::StartReplay(int skipturn, bool is_yrp) {
	if(mainGame->dInfo.isReplay)
		return false;
	skip_turn = skipturn;
	if(skip_turn < 0)
		skip_turn = 0;
	yrp = is_yrp;
	is_swapping = false;
	is_pausing = false;
	is_paused = false;
	is_restarting = false;
	if(replay_thread.joinable())
		replay_thread.join();
	if(is_yrp) {
		if(cur_replay.IsOldReplayMode())
			cur_yrp = &cur_replay;
		else
			cur_yrp = cur_replay.yrp.get();
		if(!cur_yrp)
			return false;
		replay_thread = epro::thread(OldReplayThread);
	} else
		replay_thread = epro::thread(ReplayThread);
	return true;
}
void ReplayMode::StopReplay(bool is_exiting) {
	is_pausing = false;
	is_continuing = false;
	is_closing = is_exiting;
	exit_pending = true;
	mainGame->actionSignal.Set();
	if(is_exiting && replay_thread.joinable())
		replay_thread.join();
}
void ReplayMode::SwapField() {
	if(is_paused)
		mainGame->dField.ReplaySwap();
	else
		is_swapping = true;
}
void ReplayMode::Pause(bool is_pause, bool is_step) {
	if(is_pause)
		is_pausing = true;
	else {
		if(!is_step)
			is_pausing = false;
		mainGame->actionSignal.Set();
	}
}
int ReplayMode::ReplayThread() {
	Utils::SetThreadName("ReplayMode");
	mainGame->dInfo.isReplay = true;
	const auto& replay_header = cur_replay.pheader;
	mainGame->dInfo.isFirst = true;
	mainGame->dInfo.isTeam1 = true;
	mainGame->dInfo.isRelay = !!(cur_replay.params.duel_flags & DUEL_RELAY);
	mainGame->dInfo.isSingleMode = !!(replay_header.base.flag & REPLAY_SINGLE_MODE);
	mainGame->dInfo.isHandTest = !!(replay_header.base.flag & REPLAY_HAND_TEST);
	mainGame->dInfo.compat_mode = !(replay_header.base.flag & REPLAY_LUA64);
	mainGame->dInfo.legacy_race_size = GET_CORE_VERSION_MAJOR(replay_header.base.version) < 10;
	mainGame->dInfo.team1 = cur_replay.GetPlayersCount(0);
	mainGame->dInfo.team2 = cur_replay.GetPlayersCount(1);
	mainGame->dInfo.current_player[0] = 0;
	mainGame->dInfo.current_player[1] = 0;
	if(!mainGame->dInfo.isRelay)
		mainGame->dInfo.current_player[1] = mainGame->dInfo.team2 - 1;
	const auto& names = cur_replay.GetPlayerNames();
	const auto first_oppo_player = names.begin() + mainGame->dInfo.team1;
	mainGame->dInfo.selfnames.assign(names.begin(), first_oppo_player);
	mainGame->dInfo.opponames.assign(first_oppo_player, names.end());
	mainGame->dInfo.duel_params = cur_replay.params.duel_flags;
	mainGame->dInfo.duel_field = mainGame->GetMasterRule(mainGame->dInfo.duel_params);
	matManager.SetActiveVertices(mainGame->dInfo.HasFieldFlag(DUEL_3_COLUMNS_FIELD),
								 !mainGame->dInfo.HasFieldFlag(DUEL_SEPARATE_PZONE));
	mainGame->SetPhaseButtons();
	auto& current_stream = cur_replay.packets_stream;
	if(!current_stream.size()) {
		EndDuel();
		return 0;
	}
	
	// Download all card images before starting the replay
	std::set<uint32_t> card_codes;
	CollectReplayCardCodes(card_codes);
	DownloadReplayImages(card_codes);
	
	mainGame->dInfo.isInDuel = true;
	mainGame->dInfo.isStarted = true;
	mainGame->dInfo.checkRematch = false;
	mainGame->SetMessageWindow();

	// If the server requested a swapped replay view, apply it now.
	{
		const char* _swap_env = std::getenv("EDOPRO_REPLAY_SWAP");
		if(_swap_env && _swap_env[0] && (_swap_env[0] == '1' || _swap_env[0] == 't' || _swap_env[0] == 'T')) {
			mainGame->dField.ReplaySwap();
		}
	}
	mainGame->dInfo.turn = 0;
	mainGame->dInfo.isCatchingUp = (skip_turn > 0);
	is_continuing = true;
	skip_step = 0;
	exit_pending = false;
	current_step = 0;
	if(mainGame->dInfo.isCatchingUp)
		mainGame->gMutex.lock();

	// check pause after load
	if (is_pausing) {
		is_paused = true;
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		mainGame->actionSignal.Wait(lock);
		is_paused = false;
	}

	for(auto it = current_stream.begin(); is_continuing && !exit_pending && it != current_stream.end();) {
		is_continuing = ReplayAnalyze((*it));
		if(is_restarting) {
			mainGame->gMutex.lock();
			it = current_stream.begin();
			is_restarting = false;
			int step = current_step - 1;
			if (step < 0)
				step = 0;
			if (step == 0) {
				Pause(true, false);
				mainGame->dInfo.isInDuel = true;
				mainGame->dInfo.isStarted = true;
				mainGame->dInfo.isCatchingUp = false;
				mainGame->dField.RefreshAllCards();
				mainGame->SetMessageWindow();
				mainGame->gMutex.unlock();
			}
			skip_step = step;
			current_step = 0;
		} else
			it++;
	}
	if(mainGame->dInfo.isCatchingUp) {
		mainGame->dInfo.isCatchingUp = false;
		mainGame->dField.RefreshAllCards();
		mainGame->gMutex.unlock();
	}
	EndDuel();
	return 0;
}
void ReplayMode::EndDuel() {
	if(pduel) {
		pduel = nullptr;
	}
	if(!is_closing) {
		std::unique_lock<epro::mutex> lock(mainGame->gMutex);
		mainGame->stMessage->setText(gDataManager->GetSysString(1501).data());
		if(mainGame->wCardSelect->isVisible())
			mainGame->HideElement(mainGame->wCardSelect);
		// mainGame->PopupElement(mainGame->wMessage);
		// mainGame->actionSignal.Wait(lock);
		mainGame->WaitFrameSignal(60, lock);
		mainGame->dInfo.isInDuel = false;
		mainGame->dInfo.isStarted = false;
		mainGame->dInfo.isReplay = false;
		mainGame->dInfo.isSingleMode = false;
		mainGame->dInfo.isHandTest = false;
		mainGame->dInfo.isOldReplay = false;
		mainGame->closeDuelWindow = true;
		mainGame->closeDoneSignal.Wait(lock);
		mainGame->ShowElement(mainGame->wReplay);
		mainGame->SetMessageWindow();
		mainGame->stTip->setVisible(false);
		gSoundManager->StopSounds();
		mainGame->device->setEventReceiver(&mainGame->menuHandler);
		if(mainGame->exitAfter)
			mainGame->device->closeDevice();
	}
}
void ReplayMode::Restart(bool refresh) {
	if(pduel) {
		pduel = nullptr;
		//end_duel(pduel);
		cur_replay.Rewind();
	}
	mainGame->dInfo.isInDuel = false;
	mainGame->dInfo.isStarted = false;
	mainGame->dInfo.turn = 0;
	mainGame->dField.Clear();
	mainGame->dInfo.current_player[0] = 0;
	mainGame->dInfo.current_player[1] = 0;
	if(!mainGame->dInfo.isRelay) {
		if(mainGame->dInfo.isFirst)
			mainGame->dInfo.current_player[1] = mainGame->dInfo.team2 - 1;
		else
			mainGame->dInfo.current_player[0] = mainGame->dInfo.team1 - 1;
	}
	if (yrp && !StartDuel()) {
		EndDuel();
	}
	if(refresh) {
		mainGame->dField.RefreshAllCards();
		mainGame->dInfo.isInDuel = true;
		mainGame->dInfo.isStarted = true;
	}
	skip_turn = 0;
	is_restarting = true;
}
void ReplayMode::Undo() {
	if(mainGame->dInfo.isCatchingUp || current_step == 0)
		return;
	mainGame->dInfo.isCatchingUp = true;
	Restart(false);
	Pause(false, false);
}
bool ReplayMode::ReplayAnalyze(const CoreUtils::Packet& p) {
	is_restarting = false;
	{
		if(is_closing)
			return false;
		if(is_restarting)
			return true;
		if(is_swapping) {
			std::lock_guard<epro::mutex> lock(mainGame->gMutex);
			mainGame->dField.ReplaySwap();
			is_swapping = false;
		}
		bool pauseable = true;
		mainGame->dInfo.curMsg = p.message;
		switch (mainGame->dInfo.curMsg) {
		case MSG_RETRY: {
			if(mainGame->dInfo.isCatchingUp) {
				mainGame->dInfo.isCatchingUp = false;
				mainGame->dField.RefreshAllCards();
				mainGame->gMutex.unlock();
			}
			std::unique_lock<epro::mutex> lock(mainGame->gMutex);
			mainGame->stMessage->setText(gDataManager->GetSysString(1434).data());
			mainGame->PopupElement(mainGame->wMessage);
			mainGame->actionSignal.Wait(lock);
			return false;
		}
		case MSG_WIN: {
			if(!yrp || !cur_yrp || !(cur_yrp->pheader.base.flag & REPLAY_HAND_TEST)) {
				if (mainGame->dInfo.isCatchingUp) {
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
				DuelClient::ClientAnalyze(p);
				return false;
			}
			return true;
		}
		case MSG_START:
		case MSG_UPDATE_DATA:
		case MSG_UPDATE_CARD:
		case MSG_SET:
		case MSG_SWAP:
		case MSG_FIELD_DISABLED:
		case MSG_SUMMONING:
		case MSG_SPSUMMONING:
		case MSG_FLIPSUMMONING:
		case MSG_CHAIN_SOLVING:
		case MSG_CHAIN_SOLVED:
		case MSG_CHAIN_END:
		case MSG_RANDOM_SELECTED:
		case MSG_EQUIP:
		case MSG_UNEQUIP:
		case MSG_CARD_TARGET:
		case MSG_CANCEL_TARGET:
		case MSG_BATTLE:
		case MSG_ATTACK_DISABLED:
		case MSG_DAMAGE_STEP_START:
		case MSG_DAMAGE_STEP_END:
		case MSG_TAG_SWAP:
		case MSG_RELOAD_FIELD: {
			pauseable = false;
			break;
		}
		case MSG_NEW_TURN: {
			if(skip_turn) {
				skip_turn--;
				if(skip_turn == 0) {
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
			}
			break;
		}
		case MSG_AI_NAME: {
			const auto* pbuf = p.data();
			auto len = BufferIO::Read<uint16_t>(pbuf);
			if((len + 1u) != p.buff_size() - (sizeof(uint16_t)))
				break;
			mainGame->dInfo.opponames[0] = BufferIO::DecodeUTF8({ reinterpret_cast<const char*>(pbuf), len });
			return true;
		}
		case OLD_REPLAY_MODE:
			return true;
		}
		DuelClient::ClientAnalyze(p);
		if(pauseable) {
			current_step++;
			if(skip_step) {
				skip_step--;
				if(skip_step == 0) {
					Pause(true, false);
					mainGame->dInfo.isInDuel = true;
					mainGame->dInfo.isStarted = true;
					mainGame->dInfo.isCatchingUp = false;
					mainGame->dField.RefreshAllCards();
					mainGame->gMutex.unlock();
				}
			}
			if(is_pausing) {
				is_paused = true;
				std::unique_lock<epro::mutex> lock(mainGame->gMutex);
				mainGame->actionSignal.Wait(lock);
				is_paused = false;
			}
		}
	}
	return true;
}

}
