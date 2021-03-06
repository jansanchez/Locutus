#include "InformationManager.h"
#include "OpponentModel.h"
#include "Random.h"

using namespace UAlbertaBot;

OpeningPlan OpponentModel::predictEnemyPlan() const
{
	struct PlanInfoType
	{
		int wins;
		int games;
		double weight;
	};
	PlanInfoType planInfo[int(OpeningPlan::Size)];

	// 1. Initialize.
	for (int plan = int(OpeningPlan::Unknown); plan < int(OpeningPlan::Size); ++plan)
	{
		planInfo[plan].wins = 0;
		planInfo[plan].games = 0;
		planInfo[plan].weight = 0.0;
	}

	// 2. Gather info.
	double weight = 1.0;
	for (const GameRecord * record : _pastGameRecords)
	{
		if (_gameRecord.sameMatchup(*record))
		{
			PlanInfoType & info = planInfo[int(record->getEnemyPlan())];
			info.games += 1;
			info.weight += weight;
			weight *= 1.25;        // more recent game records are more heavily weighted
		}
	}

	// 3. Decide.
	// For now, set the most heavily weighted plan other than Unknown as the expected plan. Ignore the other info.
	OpeningPlan bestPlan = OpeningPlan::Unknown;
	double bestWeight = 0.0;
	for (int plan = int(OpeningPlan::Unknown) + 1; plan < int(OpeningPlan::Size); ++plan)
	{
		if (planInfo[plan].weight > bestWeight)
		{
			bestPlan = OpeningPlan(plan);
			bestWeight = planInfo[plan].weight;
		}
	}

	return bestPlan;
}

// Does the opponent seem to play the same strategy every game?
// If we're pretty sure, set _singleStrategy to true.
// So far, we only check the plan. We have plenty of other data that could be helpful.
void OpponentModel::considerSingleStrategy()
{
	// Gather info.
	int knownPlan = 0;
	int unknownPlan = 0;
	std::set<OpeningPlan> plansSeen;

	for (const GameRecord * record : _pastGameRecords)
	{
		if (_gameRecord.sameMatchup(*record))
		{
			OpeningPlan plan = record->getEnemyPlan();
			if (plan == OpeningPlan::Unknown)
			{
				unknownPlan += 1;
			}
			else
			{
				knownPlan += 1;
				plansSeen.insert(plan);
			}
		}
	}

	// Decide.
	// If we don't recognize the majority of plans, we're not sure.
	if (knownPlan >= 2 && plansSeen.size() == 1 && unknownPlan <= knownPlan)
	{
		_singleStrategy = true;
	}
}

// If the opponent model has collected useful information,
// set _recommendedOpening, the opening to play (or instructions for choosing it).
// This runs once before play starts, when all we know is the opponent
// and whatever the game records tell us about the opponent.
void OpponentModel::considerOpenings()
{
	struct OpeningInfoType
	{
		int sameWins;		// on the same map as this game, or following the same plan as this game
		int sameGames;
		int otherWins;		// across all other maps/plans
		int otherGames;
		double weightedWins;
		double weightedGames;

		OpeningInfoType()
			: sameWins(0)
			, sameGames(0)
			, otherWins(0)
			, otherGames(0)
			// The weighted values doesn't need to be initialized up front.
		{
		}
	};

	int totalWins = 0;
	int totalGames = 0;
	std::map<std::string, OpeningInfoType> openingInfo;		// opening name -> opening info
	OpeningInfoType planInfo;								// summary of the recorded enemy plans

	// Gather basic information from the game records.
	for (const GameRecord * record : _pastGameRecords)
	{
		if (_gameRecord.sameMatchup(*record))
		{
			++totalGames;
			if (record->getWin())
			{
				++totalWins;
			}
			OpeningInfoType & info = openingInfo[record->getOpeningName()];
			if (record->getMapName() == BWAPI::Broodwar->mapFileName())
			{
				info.sameGames += 1;
				if (record->getWin())
				{
					info.sameWins += 1;
				}
			}
			else
			{
				info.otherGames += 1;
				if (record->getWin())
				{
					info.otherWins += 1;
				}
			}
			if (record->getExpectedEnemyPlan() == record->getEnemyPlan())
			{
				// The plan was recorded as correctly predicted in that game.
				planInfo.sameGames += 1;
				if (record->getWin())
				{
					planInfo.sameWins += 1;
				}
			}
			else
			{
				// The plan was not correctly predicted.
				planInfo.otherGames += 1;
				if (record->getWin())
				{
					planInfo.otherWins += 1;
				}
			}
		}
	}

	UAB_ASSERT(totalWins == planInfo.sameWins + planInfo.otherWins, "bad total");
	UAB_ASSERT(totalGames == planInfo.sameGames + planInfo.otherGames, "bad total");

	OpeningPlan enemyPlan = _expectedEnemyPlan;

    // Disable the rest for now
    _recommendedOpening = getOpeningForEnemyPlan(enemyPlan);
    return;										

	// For the first games, stick to the counter openings based on the predicted plan.
	if (totalGames <= 5)
	{
		_recommendedOpening = getOpeningForEnemyPlan(enemyPlan);
		return;										// with or without expected play
	}

	UAB_ASSERT(totalGames > 0 && totalWins >= 0, "bad total");
	UAB_ASSERT(openingInfo.size() > 0 && int(openingInfo.size()) <= totalGames, "bad total");

	// If we keep winning, stick to the winning track.
	if (totalWins == totalGames ||
		_singleStrategy && planInfo.sameWins > 0 && planInfo.sameWins == planInfo.sameGames)   // Unknown plan is OK
	{
		_recommendedOpening = getOpeningForEnemyPlan(enemyPlan);
		return;										// with or without expected play
	}
	
	// Randomly choose any opening that always wins, or always wins on this map.
	// This bypasses the map weighting below.
	// The algorithm is reservoir sampling in the simplest case, with reservoir size = 1.
	// It gives equal probabilities without remembering all the elements.
	std::string alwaysWins;
	double nAlwaysWins = 0.0;
	std::string alwaysWinsOnThisMap;
	double nAlwaysWinsOnThisMap = 0.0;
	for (auto item : openingInfo)
	{
		const OpeningInfoType & info = item.second;
		if (info.sameWins + info.otherWins > 0 && info.sameWins + info.otherWins == info.sameGames + info.otherGames)
		{
			nAlwaysWins += 1.0;
			if (Random::Instance().flag(1.0 / nAlwaysWins))
			{
				alwaysWins = item.first;
			}
		}
		if (info.sameWins > 0 && info.sameWins == info.sameGames)
		{
			nAlwaysWinsOnThisMap += 1.0;
			if (Random::Instance().flag(1.0 / nAlwaysWinsOnThisMap))
			{
				alwaysWinsOnThisMap = item.first;
			}
		}
	}
	if (!alwaysWins.empty())
	{
		_recommendedOpening = alwaysWins;
		return;
	}
	if (!alwaysWinsOnThisMap.empty())
	{
		_recommendedOpening = alwaysWinsOnThisMap;
		return;
	}

	// Explore different actions this proportion of the time.
	// The number varies depending on the overall win rate: Explore less if we're usually winning.
	const double overallWinRate = double(totalWins) / totalGames;
	UAB_ASSERT(overallWinRate >= 0.0 && overallWinRate <= 1.0, "bad total");
	const double explorationRate = 0.05 + (1.0 - overallWinRate) * 0.10;

	// Decide whether to explore, and choose which kind of exploration to do.
	// The kind of exploration is affected by totalGames. Exploration choices are:
	// The counter openings - "Counter ...".
	// The matchup openings - "matchup".
	// Any opening that this race can play - "random".
	// The opening chooser in ParseUtils knows how to interpret the strings.
	if (totalWins == 0 || Random::Instance().flag(explorationRate))
	{
		const double wrongPlanRate = double(planInfo.otherGames) / totalGames;
		// Is the predicted enemy plan likely to be right?
		if (totalGames > 30 && Random::Instance().flag(0.75))
		{
			_recommendedOpening = "random";
		}
		else if (Random::Instance().flag(0.8 * wrongPlanRate * double(std::min(totalGames, 20)) / 20.0))
		{
			_recommendedOpening = "matchup";
		}
		else
		{
			_recommendedOpening = getOpeningForEnemyPlan(enemyPlan);
		}
		return;
	}

	// Compute "weighted" win rates which combine map win rates and overall win rates, as an
	// estimate of the true win rate on this map. The estimate is ad hoc, using an assumption
	// that is sure to be wrong.
	for (auto it = openingInfo.begin(); it != openingInfo.end(); ++it)
	{
		OpeningInfoType & info = it->second;

		// Evidence provided by game results is proportional to the square root of the number of games.
		// So let's pretend that a game played on the same map provides mapPower times as much evidence
		// as a game played on another map.
		double mapPower = info.sameGames ? (info.sameGames + info.otherGames) / sqrt(info.sameGames) : 0.0;

		info.weightedWins = mapPower * info.sameWins + info.otherWins;
		info.weightedGames = mapPower * info.sameGames + info.otherGames;
	}

	// We're not exploring. Choose an opening with the best weighted win rate.
	// This is a variation on the epsilon-greedy method.
	double bestScore = -1.0;		// every opening will have a win rate >= 0
	double nBest = 1.0;
	for (auto it = openingInfo.begin(); it != openingInfo.end(); ++it)
	{
		const OpeningInfoType & info = it->second;

		double score = info.weightedGames < 0.1 ? 0.0 : info.weightedWins / info.weightedGames;

		if (score > bestScore)
		{
			_recommendedOpening = it->first;
			bestScore = score;
			nBest = 1.0;
		}
		else if (abs (score - bestScore) < 0.0001)
		{
			// We choose randomly among openings with essentially equal score, using reservoir sampling.
			nBest += 1.0;
			if (Random::Instance().flag(1.0 / nBest))
			{
				_recommendedOpening = it->first;
			}
		}
	}
}

// Possibly update the expected enemy plan.
// This runs later in the game, when we may have more information.
void OpponentModel::reconsiderEnemyPlan()
{
	if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
	{
		// We already know the actual plan. No need to form an expectation.
		return;
	}

	if (!_gameRecord.getEnemyIsRandom() || BWAPI::Broodwar->enemy()->getRace() == BWAPI::Races::Unknown)
	{
		// For now, we only update the expected plan if the enemy went random
		// and we have now learned its race. 
		// The new information should narrow down the possibilities.
		return;
	}

	// Don't reconsider too often.
	if (BWAPI::Broodwar->getFrameCount() % 12 != 8)
	{
		return;
	}

	// We set the new expected plan even if it is Unknown. Better to know that we don't know.
	_expectedEnemyPlan = predictEnemyPlan();
}

// If it seems appropriate to try to steal the enemy's gas, note that.
// We randomly steal gas at a configured rate, then possibly auto-steal gas if
// we have data about the opponent suggesting it might be a good idea.
// This version runs once per game at the start. Future versions might run later,
// so they can take into account what the opponent is doing.
void OpponentModel::considerGasSteal()
{
	// Sometimes it's queuing a gas steal regardless, let's just kill it
	return;

	// 1. Random gas stealing.
	// This part really should run only once per game.
	if (Random::Instance().flag(Config::Strategy::RandomGasStealRate))
	{
		_recommendGasSteal = true;
		return;
	}

	// 2. Is auto gas stealing turned on?
	if (!Config::Strategy::AutoGasSteal)
	{
		return;
	}

	// 3. Gather data.
	// We add fictitious games saying that not stealing gas was tried once and won, and stealing gas
	// was tried twice and lost. That way we don't try stealing gas unless we lose games without;
	// it represents that stealing gas has a cost.
	int nGames = 4;           // 4 fictitious games total
	int nWins = 1;            // 1 fictitious win total
	int nStealTries = 3;      // 3 fictitious gas steals
	int nStealWins = 0;       // 3 fictitious losses on gas steal
	int nStealSuccesses = 0;  // for deciding on timing (not used yet)
	for (const GameRecord * record : _pastGameRecords)
	{
		if (_gameRecord.sameMatchup(*record))
		{
			++nGames;
			if (record->getWin())
			{
				++nWins;
			}
			if (record->getFrameScoutSentForGasSteal())
			{
				++nStealTries;
				if (record->getWin())
				{
					++nStealWins;
				}
				if (record->getGasStealHappened())
				{
					++nStealSuccesses;
				}
			}
		}
	}

	// 3. Decide.
	// We're deciding whether to TRY to steal gas, so measure whether TRYING helps.
	// Because of the fictitious games, we never divide by zero.
	int plainGames = nGames - nStealTries;
	int plainWins = nWins - nStealWins;
	double plainWinRate = double(plainWins) / plainGames;
	double stealWinRate = double(nStealWins) / nStealTries;

	double plainUCB = plainWinRate + UCB1_bound(plainGames, nGames);
	double stealUCB = stealWinRate + UCB1_bound(nStealTries, nGames);

	//BWAPI::Broodwar->printf("plain wins %d/%d -> %g steal wins %d/%d -> %g",
	//	plainWins, plainGames, plainUCB,
	//	nStealWins, nStealTries, stealUCB);

	_recommendGasSteal = stealUCB > plainUCB;
}

// Find the past game record which best matches the current game and remember it.
void OpponentModel::setBestMatch()
{
	int bestScore = -1;
	GameRecord * bestRecord = nullptr;

	for (GameRecord * record : _pastGameRecords)
	{
		int score = _gameRecord.distance(*record);
		if (score != -1 && (!bestRecord || score < bestScore))
		{
			bestScore = score;
			bestRecord = record;
		}
	}

	_bestMatch = bestRecord;
}

// We expect the enemy to follow the given opening plan.
// Recommend an opening to counter that plan.
// The counters are configured; all we have to do is name the strategy mix.
// The empty opening "" means play the regular openings, no plan recognized.
std::string OpponentModel::getOpeningForEnemyPlan(OpeningPlan enemyPlan)
{
	if (enemyPlan == OpeningPlan::Unknown)
	{
		return "";
	}
	return "Counter " + OpeningPlanString(enemyPlan);
}

// -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- -- --

OpponentModel::OpponentModel()
	: _bestMatch(nullptr)
	, _singleStrategy(false)
	, _initialExpectedEnemyPlan(OpeningPlan::Unknown)
	, _expectedEnemyPlan(OpeningPlan::Unknown)
	, _recommendGasSteal(false)
	, _worstCaseExpectedAirTech(INT_MAX)
{
	_filename = "om_" + InformationManager::Instance().getEnemyName() + ".txt";
}

// Read past game records from the opponent model file, and do initial analysis.
void OpponentModel::read()
{
	if (Config::IO::ReadOpponentModel)
	{
		std::ifstream inFile(Config::IO::ReadDir + _filename);

		// There may not be a file to read. That's OK.
		if (inFile.bad())
		{
			return;
		}

		while (inFile.good())
		{
			// NOTE We allocate records here and never free them if valid.
			//      Their lifetime is the whole game.
			GameRecord * record = new GameRecord(inFile);
			if (record->isValid())
			{
				_pastGameRecords.push_back(record);
			}
			else
			{
				delete record;
			}
		}

		inFile.close();
	}

	// Make immediate decisions that may take into account the game records.
	// The initial expected enemy plan is set only here. That's the idea.
	// The current expected enemy plan may be reset later.
	_expectedEnemyPlan = _initialExpectedEnemyPlan = predictEnemyPlan();
	considerSingleStrategy();
	considerOpenings();
	considerGasSteal();

	// Look at the previous 3 games and store the earliest frame we saw air tech
	int count = 0;
	for (auto it = _pastGameRecords.rbegin(); it != _pastGameRecords.rend() && count < 3; it++)
	{
		if (!_gameRecord.sameMatchup(**it)) continue;

		count++;

		int airTech = (*it)->getAirTechFrame();
		if (airTech > 0 && airTech < _worstCaseExpectedAirTech)
			_worstCaseExpectedAirTech = airTech;
	}

	if (_worstCaseExpectedAirTech != INT_MAX) Log().Get() << "Worst case expected air tech at frame " << _worstCaseExpectedAirTech;
}

// Write the game records to the opponent model file.
void OpponentModel::write()
{
	if (Config::IO::WriteOpponentModel)
	{
		std::ofstream outFile(Config::IO::WriteDir + _filename, std::ios::trunc);

		// If it fails, there's not much we can do about it.
		if (outFile.bad())
		{
			return;
		}

		for (auto record : _pastGameRecords)
		{
			record->write(outFile);
		}

		// The number of initial game records to skip over without rewriting.
		// In normal operation, nToSkip is 0 or 1.
		int nToSkip = 0;
		if (int(_pastGameRecords.size()) >= Config::IO::MaxGameRecords)
		{
			nToSkip = _pastGameRecords.size() - Config::IO::MaxGameRecords + 1;
		}

		// Rewrite any old records that were read in.
		// Not needed for local testing or for SSCAIT, necessary for other competitions.
		for (auto record : _pastGameRecords)
		{
			if (nToSkip > 0)
			{
				--nToSkip;
			}
			else
			{
				record->write(outFile);
			}
		}

		// And write the record of this game.
		_gameRecord.write(outFile);

		outFile.close();
	}
}

void OpponentModel::update()
{
	_planRecognizer.update();
	reconsiderEnemyPlan();

	if (Config::IO::ReadOpponentModel || Config::IO::WriteOpponentModel)
	{
		_gameRecord.update();

		// TODO the rest is turned off for now, not currently useful
		return;

		if (BWAPI::Broodwar->getFrameCount() % 32 == 31)
		{
			setBestMatch();
		}

		if (_bestMatch)
		{
			//_bestMatch->debugLog();
			//BWAPI::Broodwar->drawTextScreen(200, 10, "%cmatch %s %s", white, _bestMatch->mapName, _bestMatch->openingName);
			BWAPI::Broodwar->drawTextScreen(220, 6, "%cmatch", white);
		}
		else
		{
			BWAPI::Broodwar->drawTextScreen(220, 6, "%cno best match", white);
		}
	}
}

// Fill in the snapshot with a prediction of what the opponent may have at a given time.
void OpponentModel::predictEnemy(int lookaheadFrames, PlayerSnapshot & snap) const
{
	const int t = BWAPI::Broodwar->getFrameCount() + lookaheadFrames;

	// Use the best-match past game record if possible.
	// Otherwise, take a current snapshot and call it the prediction.
	if (_bestMatch && _bestMatch->findClosestSnapshot(t, snap))
	{
		// All done.
	}
	else
	{
		snap.takeEnemy();
	}
}

// The inferred enemy opening plan.
OpeningPlan OpponentModel::getEnemyPlan() const
{
	return _planRecognizer.getPlan();
}

// String for displaying the recognized enemy opening plan in the UI.
std::string OpponentModel::getEnemyPlanString() const
{
	return OpeningPlanString(_planRecognizer.getPlan());
}

// String for displaying the expected enemy opening plan in the UI.
std::string OpponentModel::getExpectedEnemyPlanString() const
{
	return OpeningPlanString(_expectedEnemyPlan);
}

// The recognized enemy plan, or the current expected enemy plan if none.
OpeningPlan OpponentModel::getBestGuessEnemyPlan() const
{
	if (_planRecognizer.getPlan() != OpeningPlan::Unknown)
	{
		return _planRecognizer.getPlan();
	}
	return _expectedEnemyPlan;
}

// Look through recent games and adjust our strategy weights appropriately
std::map<std::string, double> OpponentModel::getStrategyWeightFactors() const
{
	std::map<std::string, double> result;
	std::map<std::string, int> strategyCount;

	int count = 0;

	for (auto it = _pastGameRecords.rbegin(); it != _pastGameRecords.rend() && count < 20; it++)
	{
		if (!_gameRecord.sameMatchup(**it)) continue;

		count++;

		auto& strategy = (*it)->getOpeningName();

		if (result.find(strategy) == result.end())
		{
			result[strategy] = 1.0;
			strategyCount[strategy] = 0;
		}

		double factor = result[strategy];
		strategyCount[strategy] = strategyCount[strategy] + 1;

		if ((*it)->getWin())
			factor *= 1.0 + 1.6 / strategyCount[strategy];
		else
			factor *= 1.0 - 0.6 / strategyCount[strategy];

		result[strategy] = factor;
	}

	return result;
}

bool OpponentModel::expectAirTechSoon()
{
	return _worstCaseExpectedAirTech < (BWAPI::Broodwar->getFrameCount() + BWAPI::UnitTypes::Protoss_Photon_Cannon.buildTime());
}

OpponentModel & OpponentModel::Instance()
{
	static OpponentModel instance;
	return instance;
}
