#include "GameState.h"
#include "GameCallQueue.h"
#include "GameplayActions.h"
#include "ServerSpawns.h"


void OOGDraw() {
	GameState::Update();
    if (!GameState::IsGameReady()) GameplayActions::ResetNpc();
	GameCallQueue::ProcessPending();
}

void GameLoop() {
	GameState::Update();
	GameCallQueue::ProcessPending();
    GameplayActions::TickNpc();
    ServerSpawns::SettleRunFrame();
}

