//Copyright (c) 2013 Singapore-MIT Alliance for Research and Technology
//Licensed under the terms of the MIT License, as described in the file:
//   license.txt   (http://opensource.org/licenses/MIT)

#include "Conflux.hpp"

#include <algorithm>
#include <cmath>
#include <map>
#include <stdexcept>
#include <stdint.h>
#include <sstream>
#include <vector>
#include "conf/ConfigManager.hpp"
#include "conf/ConfigParams.hpp"
#include "entities/conflux/SegmentStats.hpp"
#include "entities/Entity.hpp"
#include "entities/misc/TripChain.hpp"
#include "entities/Person.hpp"
#include "entities/roles/activityRole/ActivityPerformer.hpp"
#include "entities/vehicle/VehicleBase.hpp"
#include "event/args/EventArgs.hpp"
#include "event/EventPublisher.hpp"
#include "event/SystemEvents.hpp"
#include "geospatial/Link.hpp"
#include "geospatial/RoadSegment.hpp"
#include "geospatial/streetdir/StreetDirectory.hpp"
#include "logging/Log.hpp"
#include "message/MessageBus.hpp"
#include "path/PathSetManager.hpp"
#include "util/Utils.hpp"
#include "workers/Worker.hpp"

using namespace boost;
using namespace sim_mob;
using namespace sim_mob::medium;
using namespace sim_mob::messaging;
using namespace std;

typedef Entity::UpdateStatus UpdateStatus;

namespace
{
const double INFINITESIMAL_DOUBLE = 0.000001;
const double PASSENGER_CAR_UNIT = 400.0; //cm; 4 m.
const double MAX_DOUBLE = std::numeric_limits<double>::max();
}

unsigned Conflux::updateInterval = 0;

Conflux::Conflux(MultiNode* multinode, const MutexStrategy& mtxStrat, int id, bool isLoader) :
		Agent(mtxStrat, id), multiNode(multinode), parentWorker(nullptr), currFrame(0, 0), isBoundary(false), isMultipleReceiver(false), isLoader(isLoader), tickTimeInS(
				ConfigManager::GetInstance().FullConfig().baseGranSecond())
{
}

Conflux::~Conflux()
{
	//delete all SegmentStats in this conflux
	for (UpstreamSegmentStatsMap::iterator upstreamIt = upstreamSegStatsMap.begin(); upstreamIt != upstreamSegStatsMap.end(); upstreamIt++)
	{
		const SegmentStatsList& linkSegments = upstreamIt->second;
		for (SegmentStatsList::const_iterator segIt = linkSegments.begin(); segIt != linkSegments.end(); segIt++)
		{
			safe_delete_item(*segIt);
		}
	}
	// clear person lists
	activityPerformers.clear();
	pedestrianList.clear();
	mrt.clear();
	carSharing.clear();
}

bool Conflux::isNonspatial()
{
	return true;
}

void Conflux::initialize(const timeslice& now)
{
	frame_init(now);
	//Register handlers for the bus stop agents
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		for (std::vector<SegmentStats*>::const_iterator segStatsIt = upStrmSegMapIt->second.begin(); segStatsIt != upStrmSegMapIt->second.end(); segStatsIt++)
		{
			(*segStatsIt)->registerBusStopAgents();
		}
	}
	setInitialized(true);
}

Conflux::PersonProps::PersonProps(const Person_MT* person, const Conflux* cnflx)
{
	Role<Person_MT>* role = person->getRole();
	isMoving = true;
	roleType = 0;
	if (role)
	{
		if (role->getResource())
		{
			isMoving = role->getResource()->isMoving();
		}
		roleType = role->roleType;
		VehicleBase* vehicle = role->getResource();
		if (vehicle)
		{
			vehicleLength = vehicle->getLengthCm();
		}
		else
		{
			vehicleLength = 0;
		}
	}

	lane = person->getCurrLane();
	isQueuing = person->isQueuing;
	const SegmentStats* currSegStats = person->getCurrSegStats();
	if (currSegStats)
	{
		segment = currSegStats->getRoadSegment();
		conflux = currSegStats->getParentConflux();
		segStats = currSegStats->getParentConflux()->findSegStats(segment, currSegStats->getStatsNumberInSegment()); //person->getCurrSegStats() cannot be used as it returns a const pointer
	}
	else
	{
		segment = nullptr;
		conflux = cnflx;
		segStats = nullptr;
	}
}

void Conflux::PersonProps::printProps(unsigned int personId, uint32_t frame, std::string prefix) const
{
	std::stringstream propStrm;
	propStrm << personId << "-" << frame << "-" << prefix << "-{";
	propStrm << " conflux:";
	if (conflux)
	{
		propStrm << conflux->getMultiNode()->getID();
	}
	else
	{
		propStrm << "0x0";
	}
	propStrm << " segment:";
	if (segment)
	{
		propStrm << segment->getSegmentAimsunId();
	}
	else
	{
		propStrm << "0x0";
	}
	propStrm << " segstats:";
	if (segStats)
	{
		propStrm << segStats->getStatsNumberInSegment();
	}
	else
	{
		propStrm << "0x0";
	}
	propStrm << " lane:";
	if (lane)
	{
		propStrm << lane->getLaneID();
	}
	else
	{
		propStrm << "0x0";
	}
	propStrm << " roleType:" << roleType << " isQueuing:" << isQueuing << " isMoving:" << isMoving << " }" << std::endl;
	Print() << propStrm.str();
}

void Conflux::addAgent(Person_MT* person)
{
	if (isLoader)
	{
		loadingQueue.push_back(person);
	}
	else
	{
		Role<Person_MT>* role = person->getRole();
		if (!role)
		{
			return;
		}

		switch (role->roleType)
		{
		case Role<Person_MT>::RL_DRIVER: //fall through
		case Role<Person_MT>::RL_BUSDRIVER:
		case Role<Person_MT>::RL_BIKER:
		{
			const RoadSegment* rdSeg = person->getCurrSegStats()->getRoadSegment();
			if (!rdSeg)
			{
				throw std::runtime_error("Starting road segment cannot be NULL for drivers");
			}
			// we will always add the Person to the corresponding segment stats in "lane infinity".
			SegmentStatsMap::iterator it = segmentAgents.find(rdSeg);
			if (it != segmentAgents.end())
			{
				SegmentStatsList& statsList = it->second;
				SegmentStats* rdSegStats = statsList.front(); // we will start the person at the first segment stats of the segment
				person->setCurrSegStats(rdSegStats);
				person->setCurrLane(rdSegStats->laneInfinity);
				person->distanceToEndOfSegment = rdSegStats->getLength();
				person->remainingTimeThisTick = tickTimeInS;
				rdSegStats->addAgent(rdSegStats->laneInfinity, person);
			}
			break;
		}
		case Role<Person_MT>::RL_PEDESTRIAN:
		{
			pedestrianList.push_back(person);
			break;
		}
		case Role<Person_MT>::RL_WAITBUSACTITITY:
		{
			assignPersonToBusStopAgent(person);
			break;
		}
		case Role<Person_MT>::RL_TRAINPASSENGER:
		{
			mrt.push_back(person);
			//TODO: subscribe for time based event
			break;
		}
		case Role<Person_MT>::RL_CARPASSENGER:
		{
			assignPersonToCar(person);
			break;
		}
		case Role<Person_MT>::RL_ACTIVITY:
		{
			activityPerformers.push_back(person);
			//TODO: subscribe for time based event
			break;
		}
		case Role<Person_MT>::RL_PASSENGER:
		{
			throw std::runtime_error("person cannot start as a passenger");
			break;
		}
		}
	}
}

bool Conflux::frame_init(timeslice now)
{
	messaging::MessageBus::RegisterHandler(this);
	for (UpstreamSegmentStatsMap::iterator upstreamIt = upstreamSegStatsMap.begin(); upstreamIt != upstreamSegStatsMap.end(); upstreamIt++)
	{
		const SegmentStatsList& linkSegments = upstreamIt->second;
		for (SegmentStatsList::const_iterator segIt = linkSegments.begin(); segIt != linkSegments.end(); segIt++)
		{
			(*segIt)->initializeBusStops();
		}
	}
	/**************test code insert incident *********************/

	/*************************************************************/
	return true;
}

Entity::UpdateStatus sim_mob::medium::Conflux::frame_tick(timeslice now)
{
	throw std::runtime_error("frame_tick() is not required and not implemented for Confluxes.");
}

void sim_mob::medium::Conflux::frame_output(timeslice now)
{
	throw std::runtime_error("frame_output() is not required and not implemented for Confluxes.");
}

UpdateStatus Conflux::update(timeslice frameNumber)
{
	if (!isInitialized())
	{
		frame_init(frameNumber);
		setInitialized(true);
	}

	currFrame = frameNumber;
	if (isLoader)
	{
		loadPersons();
	}
	else
	{
		resetPositionOfLastUpdatedAgentOnLanes();
		resetPersonRemTimes(); //reset the remaining times of persons in lane infinity and VQ if required.
		processAgents(); //process all agents in this conflux for this tick
		setLastUpdatedFrame(frameNumber.frame());
	}
	UpdateStatus retVal(UpdateStatus::RS_CONTINUE); //always return continue. Confluxes never die.
	return retVal;
}

void Conflux::loadPersons()
{
	unsigned int nextTickMS = (currFrame.frame() + MT_Config::getInstance().granPersonTicks) * ConfigManager::GetInstance().FullConfig().baseGranMS();
	while (!loadingQueue.empty())
	{
		Person_MT* person = loadingQueue.front();
		loadingQueue.pop_front();
		Conflux* conflux = Conflux::findStartingConflux(person, nextTickMS);
		if (conflux)
		{
			messaging::MessageBus::PostMessage(conflux, MSG_PERSON_LOAD, messaging::MessageBus::MessagePtr(new PersonMessage(person)));
		}
	}
}

void Conflux::processAgents()
{
	PersonList orderedPersons;
	getAllPersonsUsingTopCMerge(orderedPersons); //merge on-road agents of this conflux into a single list
	orderedPersons.insert(orderedPersons.end(), activityPerformers.begin(), activityPerformers.end()); // append activity performers
	orderedPersons.insert(orderedPersons.end(), pedestrianList.begin(), pedestrianList.end()); // append pedestrians
	for (PersonList::iterator personIt = orderedPersons.begin(); personIt != orderedPersons.end(); personIt++) //iterate and update all persons
	{
		updateAgent(*personIt);
	}
	updateBusStopAgents(); //finally update bus stop agents in this conflux
}

void Conflux::updateAgent(Person_MT* person)
{
	if (person->getLastUpdatedFrame() < currFrame.frame())
	{	//if the person is being moved for the first time in this tick, reset person's remaining time to full tick size
		person->remainingTimeThisTick = tickTimeInS;
	}

	//let the person know which worker is (indirectly) managing him
	person->currWorkerProvider = parentWorker;

	//capture person info before update
	PersonProps beforeUpdate(person, this);

	//let the person move
	UpdateStatus res = movePerson(currFrame, person);

	//kill person if he's DONE
	if (res.status == UpdateStatus::RS_DONE)
	{
		killAgent(person, beforeUpdate);
		return;
	}

	//capture person info after update
	PersonProps afterUpdate(person, this);

	//perform house keeping
	housekeep(beforeUpdate, afterUpdate, person);

	//update person's handler registration with MessageBus, if required
	updateAgentContext(beforeUpdate, afterUpdate, person);
}

void Conflux::housekeep(PersonProps& beforeUpdate, PersonProps& afterUpdate, Person_MT* person)
{
	//if the person was in an activity and is in a Trip/SubTrip after update
	if (beforeUpdate.roleType == Role<Person_MT>::RL_ACTIVITY && afterUpdate.roleType != Role<Person_MT>::RL_ACTIVITY)
	{
		// if the person has changed from an Activity to the current Trip/SubTrip during this tick,
		// remove this person from the activityPerformers list
		std::deque<Person_MT*>::iterator pIt = std::find(activityPerformers.begin(), activityPerformers.end(), person);
		if (pIt != activityPerformers.end())
		{
			activityPerformers.erase(pIt);
		}

		//if the person has switched to Pedestrian role, put the person in that list
		if (afterUpdate.roleType == Role<Person_MT>::RL_PEDESTRIAN)
		{
			PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
			if (pIt == pedestrianList.end())
			{
				pedestrianList.push_back(person);
			}
			return; //we are done here.
		}
	}

	//perform person's role related handling
	//we first handle roles which are off the road
	switch (afterUpdate.roleType)
	{
	case Role<Person_MT>::RL_WAITBUSACTITITY:
	case Role<Person_MT>::RL_TRAINPASSENGER:
	case Role<Person_MT>::RL_CARPASSENGER:
	{
		return; //would have already been handled
	}
	case Role<Person_MT>::RL_ACTIVITY:
	{	//activity role specific handling
		// if role is ActivityPerformer after update
		if (beforeUpdate.roleType == Role<Person_MT>::RL_ACTIVITY)
		{
			// if the role was ActivityPerformer before the update as well, do
			// nothing. It is also possible that the person has changed from
			// one activity to another. Do nothing even in this case.
		}
		else
		{
			if (beforeUpdate.roleType == Role<Person_MT>::RL_PEDESTRIAN)
			{
				PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
				if (pIt != pedestrianList.end())
				{
					pedestrianList.erase(pIt);
				}
			}
			else if (beforeUpdate.lane)
			{
				// the person is currently in an activity, was on a Trip
				// before this tick and was not in a virtual queue (because beforeUpdate.lane is not null)
				// Remove this person from the network and add him to the activity performers list.
				beforeUpdate.segStats->dequeue(person, beforeUpdate.lane, beforeUpdate.isQueuing, beforeUpdate.vehicleLength);
			}
			activityPerformers.push_back(person);
		}
		return;
	}
	case Role<Person_MT>::RL_PEDESTRIAN:
	{
		if (beforeUpdate.roleType == Role<Person_MT>::RL_PEDESTRIAN)
		{
			return;
		}
		break;
	}
	case Role<Person_MT>::RL_BUSDRIVER:
	{
		if (beforeUpdate.isMoving && !afterUpdate.isMoving)
		{
			//if the vehicle stopped moving during the latest update (which
			//indicates that the bus has started serving a stop) we remove the bus from
			//segment stats
			//NOTE: the bus driver we remove here would have already been added
			//to the BusStopAgent corresponding to the stop currently served by
			//the bus driver.
			if (beforeUpdate.lane)
			{
				beforeUpdate.segStats->dequeue(person, beforeUpdate.lane, beforeUpdate.isQueuing, beforeUpdate.vehicleLength);
			}
			//if the bus driver started moving from a virtual queue, his beforeUpdate.lane will be null.
			//However, since he is already into a bus stop (afterUpdate.isMoving is false) we need not
			// add this bus driver to the new seg stats. So we must return from here in any case.
			return;
		}
		else if (!beforeUpdate.isMoving && afterUpdate.isMoving)
		{
			//if the vehicle has started moving during the latest update (which
			//indicates that the bus has finished serving a stop and is getting
			//back into the road network) we add the bus driver to the new segment
			//stats
			//NOTE: the bus driver we add here would have already been removed
			//from the BusStopAgent corresponding to the stop served by the
			//bus driver.
			if (afterUpdate.lane)
			{
				afterUpdate.segStats->addAgent(afterUpdate.lane, person);
				return;
			}
			else
			{
				//the bus driver moved out of a stop and got added into a VQ.
				//we need to add the bus driver to the virtual queue here
				person->distanceToEndOfSegment = afterUpdate.segStats->getLength();
				afterUpdate.segStats->getParentConflux()->pushBackOntoVirtualQueue(afterUpdate.segment->getLink(), person);
				return;
			}
		}
		else if (!beforeUpdate.isMoving && !afterUpdate.isMoving && beforeUpdate.segStats != afterUpdate.segStats)
		{
			//The bus driver has moved out of one stop and entered another within the same tick
			//we should not add the bus driver into the new segstats because he is already at the bus stop of that stats
			//we simply return in this case
			return;
		}
		break;
	}
	}

	//now we consider roles on the road
	//note: A person is in the virtual queue or performing and activity if beforeUpdate.lane is null
	if (!beforeUpdate.lane)
	{ 	//if the person was in virtual queue or was performing an activity
		if (afterUpdate.lane)
		{ 	//if the person has moved to another lane (possibly even to laneInfinity if he was performing activity) in some segment
			afterUpdate.segStats->addAgent(afterUpdate.lane, person);
		}
		else
		{
			if (beforeUpdate.segStats != afterUpdate.segStats)
			{
				// the person must've have moved to another virtual queue - which is not possible if the virtual queues are processed
				// after all conflux updates
				std::stringstream debugMsgs;
				debugMsgs << "Error: Person has moved from one virtual queue to another. " << "\n Person " << person->getId() << "|Frame: " << currFrame.frame()
						<< "|Conflux: " << this->multiNode->getID() << "|segBeforeUpdate: " << beforeUpdate.segment->getSegmentAimsunId() << "|segAfterUpdate: "
						<< afterUpdate.segment->getSegmentAimsunId();
				throw std::runtime_error(debugMsgs.str());
			}
			else
			{
				// this is typically the person who was not accepted by the next lane in the next segment.
				// we push this person back to the same virtual queue and let him update in the next tick.
				person->distanceToEndOfSegment = afterUpdate.segStats->getLength();
				afterUpdate.segStats->getParentConflux()->pushBackOntoVirtualQueue(afterUpdate.segment->getLink(), person);
			}
		}
	}
	else if ((beforeUpdate.segStats != afterUpdate.segStats) /*if the person has moved to another segment*/
	|| (beforeUpdate.lane == beforeUpdate.segStats->laneInfinity && beforeUpdate.lane != afterUpdate.lane) /* or if the person has moved out of lane infinity*/)
	{
		if (beforeUpdate.roleType != Role<Person_MT>::RL_ACTIVITY)
		{
			// the person could have been an activity performer in which case beforeUpdate.segStats would be just null
			beforeUpdate.segStats->dequeue(person, beforeUpdate.lane, beforeUpdate.isQueuing, beforeUpdate.vehicleLength);
		}
		if (afterUpdate.lane)
		{
			afterUpdate.segStats->addAgent(afterUpdate.lane, person);
		}
		else
		{
			// we wouldn't know which lane the person has to go to if the person wants to enter a link which belongs to
			// a conflux that is not yet processed for this tick. We add this person to the virtual queue for that link here
			person->distanceToEndOfSegment = afterUpdate.segStats->getLength();
			afterUpdate.segStats->getParentConflux()->pushBackOntoVirtualQueue(afterUpdate.segment->getLink(), person);
		}
	}
	else if (beforeUpdate.segStats == afterUpdate.segStats && afterUpdate.lane == afterUpdate.segStats->laneInfinity)
	{
		//it's possible for some persons to start a new trip on the same segment where they ended the previous trip.
		beforeUpdate.segStats->dequeue(person, beforeUpdate.lane, beforeUpdate.isQueuing, beforeUpdate.vehicleLength);
		//adding the person to lane infinity for the new trip
		afterUpdate.segStats->addAgent(afterUpdate.lane, person);
	}
	else if (beforeUpdate.isQueuing != afterUpdate.isQueuing)
	{
		//the person has joined the queuing part of the same segment stats
		afterUpdate.segStats->updateQueueStatus(afterUpdate.lane, person);
	}

	// set the position of the last updated Person in his current lane (after update)
	if (afterUpdate.lane && afterUpdate.lane != afterUpdate.segStats->laneInfinity)
	{
		//if the person did not end up in a VQ and his lane is not lane infinity of segAfterUpdate
		double lengthToVehicleEnd = person->distanceToEndOfSegment + person->getRole()->getResource()->getLengthCm();
		afterUpdate.segStats->setPositionOfLastUpdatedAgentInLane(lengthToVehicleEnd, afterUpdate.lane);
	}
}

void Conflux::updateAgentContext(PersonProps& beforeUpdate, PersonProps& afterUpdate, Person_MT* person) const
{
	if (beforeUpdate.conflux && afterUpdate.conflux && beforeUpdate.conflux != afterUpdate.conflux)
	{
		MessageBus::ReRegisterHandler(person, afterUpdate.conflux->GetContext());
	}
}

void Conflux::processVirtualQueues()
{
	int counter = 0;
	{
		boost::unique_lock<boost::recursive_mutex> lock(mutexOfVirtualQueue);
		//sort the virtual queues before starting to move agents for this tick
		for (VirtualQueueMap::iterator i = virtualQueuesMap.begin(); i != virtualQueuesMap.end(); i++)
		{
			counter = i->second.size();
			sortPersonsDecreasingRemTime(i->second);
			while (counter > 0)
			{
				Person_MT* p = i->second.front();
				i->second.pop_front();
				updateAgent(p);
				counter--;
			}
		}
	}
}

double Conflux::getSegmentSpeed(SegmentStats* segStats) const
{
	return segStats->getSegSpeed(true);
}

/*
 * This function resets the remainingTime of persons who remain in lane infinity for more than 1 tick.
 * Note: This may include
 * 1. newly starting persons who (were supposed to, but) did not get added to the simulation
 * in the previous tick due to traffic congestion in their starting segment.
 * 2. Persons who got added to and remained virtual queue in the previous tick
 */
void Conflux::resetPersonRemTimes()
{
	SegmentStats* segStats = nullptr;
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		for (std::vector<SegmentStats*>::const_iterator segStatsIt = upStrmSegMapIt->second.begin(); segStatsIt != upStrmSegMapIt->second.end(); segStatsIt++)
		{
			segStats = *segStatsIt;
			PersonList& personsInLaneInfinity = segStats->getPersons(segStats->laneInfinity);
			for (PersonList::iterator personIt = personsInLaneInfinity.begin(); personIt != personsInLaneInfinity.end(); personIt++)
			{
				if ((*personIt)->getLastUpdatedFrame() < currFrame.frame())
				{
					//if the person is going to be moved for the first time in this tick
					(*personIt)->remainingTimeThisTick = tickTimeInS;
				}
			}
		}
	}

	{
		boost::unique_lock<boost::recursive_mutex> lock(mutexOfVirtualQueue);
		for (VirtualQueueMap::iterator vqIt = virtualQueuesMap.begin(); vqIt != virtualQueuesMap.end(); vqIt++)
		{
			PersonList& personsInVQ = vqIt->second;
			for (PersonList::iterator pIt = personsInVQ.begin(); pIt != personsInVQ.end(); pIt++)
			{
				if ((*pIt)->getLastUpdatedFrame() < currFrame.frame())
				{
					//if the person is going to be moved for the first time in this tick
					(*pIt)->remainingTimeThisTick = tickTimeInS;
				}
			}
		}
	}
}

unsigned int Conflux::resetOutputBounds()
{
	boost::unique_lock<boost::recursive_mutex> lock(mutexOfVirtualQueue);
	unsigned int vqCount = 0;
	vqBounds.clear();
	Link* lnk = nullptr;
	SegmentStats* segStats = nullptr;
	int outputEstimate = 0;
	for (VirtualQueueMap::iterator i = virtualQueuesMap.begin(); i != virtualQueuesMap.end(); i++)
	{
		lnk = i->first;
		segStats = upstreamSegStatsMap.at(lnk).front();
		/** In DynaMIT, the upper bound to the space in virtual queue was set based on the number of empty spaces
		 the first segment of the downstream link (the one with the vq is attached to it) is going to create in this tick according to the outputFlowRate*tick_size.
		 This would ideally underestimate the space available in the next segment, as it doesn't account for the empty spaces the segment already has.
		 Therefore the virtual queues are most likely to be cleared by the end of that tick.
		 [1] But with short segments, we noticed that this over estimated the space and left a considerably large amount of vehicles remaining in vq.
		 Therefore, as per Yang Lu's suggestion, we are replacing computeExpectedOutputPerTick() calculation with existing number of empty spaces on the segment.
		 [2] Another reason for vehicles to remain in vq is that in mid-term, we currently process the new vehicles (i.e.trying to get added to the network from lane infinity),
		 before we process the virtual queues. Therefore the space that we computed to be for vehicles in virtual queues, would have been already occupied by the new vehicles
		 by the time the vehicles in virtual queues try to get added.
		 **/
		//outputEstimate = segStats->computeExpectedOutputPerTick();
		/** using ceil here, just to avoid short segments returning 0 as the total number of vehicles the road segment can hold i.e. when segment is shorter than a car**/
		int num_emptySpaces = std::ceil(segStats->getRoadSegment()->getPolylineLength() * segStats->getRoadSegment()->getLanes().size() / PASSENGER_CAR_UNIT)
				- segStats->numMovingInSegment(true) - segStats->numQueuingInSegment(true);
		outputEstimate = (num_emptySpaces >= 0) ? num_emptySpaces : 0;
		/** we are decrementing the number of agents in lane infinity (of the first segment) to overcome problem [2] above**/
		outputEstimate = outputEstimate - segStats->numAgentsInLane(segStats->laneInfinity);
		outputEstimate = (outputEstimate > 0 ? outputEstimate : 0);
		vqBounds.insert(std::make_pair(lnk, (unsigned int) outputEstimate));
		vqCount += virtualQueuesMap.at(lnk).size();
	}			//loop

	if (vqBounds.empty() && !virtualQueuesMap.empty())
	{
		Print() << boost::this_thread::get_id() << "," << this->multiNode->getID() << " vqBounds.empty()" << std::endl;
	}
	return vqCount;
}

bool Conflux::hasSpaceInVirtualQueue(Link* lnk)
{
	bool res = false;
	{
		boost::unique_lock<boost::recursive_mutex> lock(mutexOfVirtualQueue);
		try
		{
			res = (vqBounds.at(lnk) > virtualQueuesMap.at(lnk).size());
		} catch (std::out_of_range& ex)
		{
			std::stringstream debugMsgs;
			debugMsgs << boost::this_thread::get_id() << " out_of_range exception occured in hasSpaceInVirtualQueue()" << "|Conflux: "
					<< this->multiNode->getID() << "|lnk:[" << lnk->getStart()->getID() << "," << lnk->getEnd()->getID() << "]" << "|lnk:" << lnk
					<< "|virtualQueuesMap.size():" << virtualQueuesMap.size() << "|elements:";
			for (std::map<Link*, std::deque<Person_MT*> >::iterator i = virtualQueuesMap.begin(); i != virtualQueuesMap.end(); i++)
			{
				debugMsgs << " ([" << i->first->getStart()->getID() << "," << i->first->getEnd()->getID() << "]:" << i->first << "," << i->second.size()
						<< "),";
			}
			debugMsgs << "|\nvqBounds.size(): " << vqBounds.size() << std::endl;
			throw std::runtime_error(debugMsgs.str());
		}
	}
	return res;
}

void Conflux::pushBackOntoVirtualQueue(Link* lnk, Person_MT* p)
{
	boost::unique_lock<boost::recursive_mutex> lock(mutexOfVirtualQueue);
	virtualQueuesMap.at(lnk).push_back(p);
}

void Conflux::updateAndReportSupplyStats(timeslice frameNumber)
{
	const ConfigManager& cfg = ConfigManager::GetInstance();
	bool outputEnabled = cfg.CMakeConfig().OutputEnabled();
	bool updateThisTick = ((frameNumber.frame() % updateInterval) == 0);
	for (UpstreamSegmentStatsMap::iterator upstreamIt = upstreamSegStatsMap.begin(); upstreamIt != upstreamSegStatsMap.end(); upstreamIt++)
	{
		const SegmentStatsList& linkSegments = upstreamIt->second;
		for (SegmentStatsList::const_iterator segIt = linkSegments.begin(); segIt != linkSegments.end(); segIt++)
		{
			if (updateThisTick && outputEnabled)
			{
				Log() << (*segIt)->reportSegmentStats(frameNumber.frame() / updateInterval);
			}
			(*segIt)->updateLaneParams(frameNumber);
		}
	}
}

void Conflux::killAgent(Person_MT* person, PersonProps& beforeUpdate)
{
	SegmentStats* prevSegStats = beforeUpdate.segStats;
	const Lane* prevLane = beforeUpdate.lane;
	bool wasQueuing = beforeUpdate.isQueuing;
	double vehicleLength = beforeUpdate.vehicleLength;
	Role<Person_MT>::Type personRoleType = Role<Person_MT>::RL_UNKNOWN;
	if (person->getRole())
	{
		personRoleType = person->getRole()->roleType;
	}
	switch (personRoleType)
	{
	case Role<Person_MT>::RL_ACTIVITY:
	{
		PersonList::iterator pIt = std::find(activityPerformers.begin(), activityPerformers.end(), person);
		if (pIt != activityPerformers.end())
		{
			activityPerformers.erase(pIt);
		}
		break;
	}
	case Role<Person_MT>::RL_PEDESTRIAN:
	{
		PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
		if (pIt != pedestrianList.end())
		{
			pedestrianList.erase(pIt);
		}
		if (person->getNextLinkRequired())
		{
			return;
		}
		break;
	}
	case Role<Person_MT>::RL_DRIVER:
	{
		//It is possible that a driver is getting removed silently because
		//a path could not be established for his current sub trip.
		//In this case, the role will be Driver but the prevLane and prevSegStats will be NULL
		//if the person's previous trip chain item is an Activity.
		//TODO: There might be other weird scenarios like this, to be taken care of.
		PersonList::iterator pIt = std::find(activityPerformers.begin(), activityPerformers.end(), person);
		if (pIt != activityPerformers.end())
		{
			activityPerformers.erase(pIt);
		} //Check if he was indeed an activity performer and erase him
		else if (prevLane)
		{
			bool removed = prevSegStats->removeAgent(prevLane, person, wasQueuing, vehicleLength);
			if (!removed)
			{
				throw std::runtime_error("Conflux::killAgent(): Attempt to remove non-existent person in Lane");
			}
		}
		break;
	}
	default: //applies for any other vehicle in a lane (Biker, Busdriver etc.)
	{
		if (prevLane)
		{
			bool removed = prevSegStats->removeAgent(prevLane, person, wasQueuing, vehicleLength);
			//removed can be false only in the case of BusDrivers at the moment.
			//This is because a BusDriver could have been dequeued from prevLane in the previous tick and be added to his
			//last bus stop. When he has finished serving the stop, the BusDriver is done. He will be killed here. However,
			//since he was already dequeued, we can't find him in prevLane now.
			//It is an error only if removed is false and the role is not BusDriver.
			if (!removed && personRoleType != Role<Person_MT>::RL_BUSDRIVER)
			{
				throw std::runtime_error("Conflux::killAgent(): Attempt to remove non-existent person in Lane");
			}
		}
		break;
	}
	}

	//Print()<<"agent is removed by conflux:"<<person->getId()<<"|role:"<<(int)personRoleType<<std::endl;
	person->currWorkerProvider = nullptr;
	parentWorker->remEntity(person);
	parentWorker->scheduleForRemoval(person);
}

void Conflux::resetPositionOfLastUpdatedAgentOnLanes()
{
	for (UpstreamSegmentStatsMap::iterator upstreamIt = upstreamSegStatsMap.begin(); upstreamIt != upstreamSegStatsMap.end(); upstreamIt++)
	{
		const SegmentStatsList& linkSegments = upstreamIt->second;
		for (SegmentStatsList::const_iterator segIt = linkSegments.begin(); segIt != linkSegments.end(); segIt++)
		{
			(*segIt)->resetPositionOfLastUpdatedAgentOnLanes();
		}
	}
}

SegmentStats* Conflux::findSegStats(const RoadSegment* rdSeg, uint16_t statsNum)
{
	if (!rdSeg || statsNum == 0)
	{
		return nullptr;
	}
	SegmentStatsList& statsList = segmentAgents.find(rdSeg)->second;
	if (statsList.size() < statsNum)
	{
		return nullptr;
	}
	SegmentStatsList::iterator statsIt = statsList.begin();
	if (statsNum == 1)
	{
		return (*statsIt);
	}
	std::advance(statsIt, (statsNum - 1));
	return (*statsIt);
}

void Conflux::setLinkTravelTimes(double travelTime, const Link* link)
{
	std::map<const Link*, LinkTravelTimes>::iterator itTT = linkTravelTimesMap.find(link);
	if (itTT != linkTravelTimesMap.end())
	{
		itTT->second.personCnt = itTT->second.personCnt + 1;
		itTT->second.linkTravelTime_ = itTT->second.linkTravelTime_ + travelTime;
	}
	else
	{
		LinkTravelTimes tTimes(travelTime, 1);
		linkTravelTimesMap.insert(std::make_pair(link, tTimes));
	}
}

bool Conflux::callMovementFrameInit(timeslice now, Person_MT* person)
{
	//register the person as a message handler if required
	if (!person->GetContext())
	{
		messaging::MessageBus::RegisterHandler(person);
	}

	//Agents may be created with a null Role and a valid trip chain
	if (!person->getRole())
	{
		//TODO: This UpdateStatus has a "prevParams" and "currParams" that should
		//      (one would expect) be dealt with. Where does this happen?
		UpdateStatus res = person->checkTripChain();

		//Reset the start time (to the current time tick) so our dispatcher doesn't complain.
		person->setStartTime(now.ms());

		//Nothing left to do?
		if (res.status == UpdateStatus::RS_DONE)
		{
			return false;
		}
	}
	//Failsafe: no Role at all?
	if (!person->getRole())
	{
		std::stringstream debugMsgs;
		debugMsgs << "Person " << this->getId() << " has no Role.";
		throw std::runtime_error(debugMsgs.str());
	}

	//Get an UpdateParams instance.
	//TODO: This is quite unsafe, but it's a relic of how Person::update() used to work.
	//      We should replace this eventually (but this will require a larger code cleanup).
	person->getRole()->make_frame_tick_params(now);

	//Now that the Role has been fully constructed, initialize it.
	if (person->getRole())
	{
		person->getRole()->Movement()->frame_init();
		if (person->isToBeRemoved())
		{
			return false;
		} //if agent initialization fails, person is set to be removed
	}

	return true;
}

void Conflux::HandleMessage(messaging::Message::MessageType type, const messaging::Message& message)
{
	switch (type)
	{
	case MSG_PEDESTRIAN_TRANSFER_REQUEST:
	{
		const PersonMessage& msg = MSG_CAST(PersonMessage, message);
		msg.person->currWorkerProvider = parentWorker;
		messaging::MessageBus::ReRegisterHandler(msg.person, GetContext());
		pedestrianList.push_back(msg.person);
		break;
	}
	case MSG_INSERT_INCIDENT:
	{
		//pathsetLogger << "Conflux received MSG_INSERT_INCIDENT" << std::endl;
		const InsertIncidentMessage & msg = MSG_CAST(InsertIncidentMessage, message);
		//change the flow rate of the segment
		BOOST_FOREACH(SegmentStats* stat, msg.stats)
		{
			Conflux::insertIncident(stat, msg.newFlowRate);
		}
		break;
	}
	case MSG_MRT_PASSENGER_TELEPORTATION:
	{
		const PersonMessage& msg = MSG_CAST(PersonMessage, message);
		msg.person->currWorkerProvider = parentWorker;
		messaging::MessageBus::ReRegisterHandler(msg.person, GetContext());
		mrt.push_back(msg.person);
		DailyTime time = msg.person->currSubTrip->endTime;
		msg.person->getRole()->setTravelTime(time.getValue());
		unsigned int tick = ConfigManager::GetInstance().FullConfig().baseGranMS();
		unsigned int offset = time.getValue() / tick;
		//TODO: compute time to be expired and send message to self
		messaging::MessageBus::PostMessage(this, MSG_WAKE_UP, messaging::MessageBus::MessagePtr(new PersonMessage(msg.person)), false, offset);
		break;
	}
	case MSG_WAKE_UP:
	{
		const PersonMessage& msg = MSG_CAST(PersonMessage, message);
		PersonList::iterator pIt = std::find(mrt.begin(), mrt.end(), msg.person);
		if (pIt == mrt.end())
		{
			throw std::runtime_error("Person not found in MRT list");
		}
		mrt.erase(pIt);
		//switch to next trip chain item
		switchTripChainItem(msg.person);
		break;
	}
	case MSG_WAKEUP_CAR_PASSENGER_TELEPORTATION:
	{
		const PersonMessage& msg = MSG_CAST(PersonMessage, message);
		PersonList::iterator pIt = std::find(carSharing.begin(), carSharing.end(), msg.person);
		if (pIt == carSharing.end())
		{
			throw std::runtime_error("Person not found in Car list");
		}
		carSharing.erase(pIt);
		//switch to next trip chain item
		switchTripChainItem(msg.person);
		break;
	}
	case MSG_PERSON_LOAD:
	{
		const PersonMessage& msg = MSG_CAST(PersonMessage, message);
		addAgent(msg.person);
	}
	default:
		break;
	}
}

void Conflux::collectTravelTime(Person_MT* person)
{
	if (person && person->getRole())
	{
		person->getRole()->collectTravelTime();
	}
}

Entity::UpdateStatus Conflux::switchTripChainItem(Person_MT* person)
{
	collectTravelTime(person);
	Entity::UpdateStatus retVal = person->checkTripChain();
	if (retVal.status == UpdateStatus::RS_DONE)
	{
		return retVal;
	}
	Role<Person_MT>* personRole = person->getRole();
	person->setStartTime(currFrame.ms());
	if (personRole && personRole->roleType == Role<Person_MT>::RL_WAITBUSACTITITY)
	{
		assignPersonToBusStopAgent(person);
		PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
		if (pIt != pedestrianList.end())
		{
			pedestrianList.erase(pIt);
		}
		return retVal;
	}

	if (personRole && personRole->roleType == Role<Person_MT>::RL_TRAINPASSENGER)
	{
		assignPersonToMRT(person);
		PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
		if (pIt != pedestrianList.end())
		{
			pedestrianList.erase(pIt);
		}
		return retVal;
	}

	if (personRole && personRole->roleType == Role<Person_MT>::RL_CARPASSENGER)
	{
		assignPersonToCar(person);
		PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
		if (pIt != pedestrianList.end())
		{
			pedestrianList.erase(pIt);
		}
		return retVal;
	}

	if (personRole && personRole->roleType == Role<Person_MT>::RL_PEDESTRIAN)
	{
		PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
		if (pIt == pedestrianList.end())
		{
			pedestrianList.push_back(person);
		}
		return retVal;
	}

	if ((*person->currTripChainItem)->itemType == TripChainItem::IT_ACTIVITY)
	{
		//IT_ACTIVITY as of now is just a matter of waiting for a period of time(between its start and end time)
		//since start time of the activity is usually later than what is configured initially,
		//we have to make adjustments so that it waits for exact amount of time
		Activity* acItem = dynamic_cast<Activity*>((*person->currTripChainItem));
		ActivityPerformer<Person_MT> *ap = dynamic_cast<ActivityPerformer<Person_MT>*>(personRole);
		ap->setActivityStartTime(DailyTime(currFrame.ms() + ConfigManager::GetInstance().FullConfig().baseGranMS()));
		ap->setActivityEndTime(
				DailyTime(
						currFrame.ms() + ConfigManager::GetInstance().FullConfig().baseGranMS()
								+ ((*person->currTripChainItem)->endTime.getValue() - (*person->currTripChainItem)->startTime.getValue())));
		ap->setLocation(acItem->destination.node_);
	}
	if (callMovementFrameInit(currFrame, person))
	{
		person->setInitialized(true);
	}
	else
	{
		return UpdateStatus::Done;
	}
	return retVal;
}

Entity::UpdateStatus Conflux::callMovementFrameTick(timeslice now, Person_MT* person)
{
	Role<Person_MT>* personRole = person->getRole();
	if (person->isResetParamsRequired())
	{
		personRole->make_frame_tick_params(now);
		person->setResetParamsRequired(false);
	}
	person->setLastUpdatedFrame(currFrame.frame());

	Entity::UpdateStatus retVal = UpdateStatus::Continue;

	/*
	 * The following loop guides the movement of the person by invoking the movement facet of the person's role one or more times
	 * until the remainingTimeThisTick of the person is expired.
	 * The frame tick of the movement facet returns when one of the following conditions are true. These are handled by case distinction.
	 *
	 * 1. Driver's frame_tick() has displaced the person to the maximum distance that the person can move in the full tick duration.
	 * This case identified by checking if the remainingTimeThisTick of the person is 0.
	 * If remainingTimeThisTick == 0 we break off from the while loop.
	 * The person's location is updated in the conflux that it belongs to. If the person has to be removed from the simulation, he is.
	 *
	 * 2. The person has reached the end of a link.
	 * This case is identified by checking requestedNextSegment which indicates that the role has requested permission to move to the next segment in a new link in its path.
	 * The requested next segment will be assigned a segment by the mid-term driver iff the driver is moving into a new link.
	 * The conflux immediately grants permission by setting canMoveToNextSegment to GRANTED.
	 * If the next link is not processed for the current tick, the person is added to the virtual queue of the next conflux and the loop is broken.
	 * If the next link is processed, the loop continues. The movement role facet (driver) checks canMoveToNextSegment flag before it advances in its frame_tick.
	 *
	 * 3. The person has reached the end of the current subtrip. The loop will catch this by checking person->isToBeRemoved() flag.
	 * If the driver has reached the end of the current subtrip, the loop updates the current trip chain item of the person and change roles by calling person->checkTripChain().
	 * We also set the current segment, set the lane as lane infinity and call the movement facet of the person's role again.
	 */
	while (person->remainingTimeThisTick > 0.0)
	{
		if (!person->isToBeRemoved())
		{
			personRole->Movement()->frame_tick();
		}

		if (person->isToBeRemoved())
		{
			retVal = switchTripChainItem(person);
			if (retVal.status == UpdateStatus::RS_DONE)
			{
				return retVal;
			}
			personRole = person->getRole();
		}

		if (person->getNextLinkRequired())
		{
			Conflux* nextConflux = person->getNextLinkRequired()->getSegments().front()->getParentConflux();
			messaging::MessageBus::PostMessage(nextConflux, MSG_PEDESTRIAN_TRANSFER_REQUEST, messaging::MessageBus::MessagePtr(new PersonMessage(person)));
			person->setNextLinkRequired(nullptr);
			PersonList::iterator pIt = std::find(pedestrianList.begin(), pedestrianList.end(), person);
			if (pIt != pedestrianList.end())
			{
				pedestrianList.erase(pIt);
				person->currWorkerProvider = nullptr;
			}
			return UpdateStatus::Continue;
		}

		if (person->requestedNextSegStats)
		{
			const RoadSegment* nxtSegment = person->requestedNextSegStats->getRoadSegment();
			Conflux* nxtConflux = person->requestedNextSegStats->getParentConflux();

			// grant permission. But check whether the subsequent frame_tick can be called now.
			person->canMoveToNextSegment = Person_MT::GRANTED;
			long currentFrame = now.frame(); //frame will not be outside the range of long data type
			LaneParams* currLnParams = person->getCurrSegStats()->getLaneParams(person->getCurrLane());
			if (currentFrame > nxtConflux->getLastUpdatedFrame())
			{
				// nxtConflux is not processed for the current tick yet
				if (nxtConflux->hasSpaceInVirtualQueue(nxtSegment->getLink()) && currLnParams->getOutputCounter() > 0)
				{
					currLnParams->decrementOutputCounter();
					person->setCurrSegStats(person->requestedNextSegStats);
					person->setCurrLane(nullptr); // so that the updateAgent function will add this agent to the virtual queue
					person->requestedNextSegStats = nullptr;
					break; //break off from loop
				}
				else
				{
					person->canMoveToNextSegment = Person_MT::DENIED;
					person->requestedNextSegStats = nullptr;
				}
			}
			else if (currentFrame == nxtConflux->getLastUpdatedFrame())
			{
				// nxtConflux is processed for the current tick. Can move to the next link.
				// already handled by setting person->canMoveToNextSegment = GRANTED
				if (currLnParams->getOutputCounter() > 0)
				{
					currLnParams->decrementOutputCounter();
					person->requestedNextSegStats = nullptr;
				}
				else
				{
					person->canMoveToNextSegment = Person_MT::DENIED;
					person->requestedNextSegStats = nullptr;
				}
			}
			else
			{
				throw std::runtime_error("lastUpdatedFrame of confluxes are managed incorrectly");
			}
		}
	}
	return retVal;
}

void Conflux::callMovementFrameOutput(timeslice now, Person_MT* person)
{
	//Save the output
	if (!isToBeRemoved())
	{
		person->currRole->Movement()->frame_tick_output();
	}
}

void Conflux::reportLinkTravelTimes(timeslice frameNumber)
{
	if (ConfigManager::GetInstance().CMakeConfig().OutputEnabled())
	{
		std::map<const Link*, LinkTravelTimes>::const_iterator it = linkTravelTimesMap.begin();
		for (; it != linkTravelTimesMap.end(); ++it)
		{
			LogOut(
					"(\"linkTravelTime\"" <<","<<frameNumber.frame() <<","<<it->first->getLinkId() <<",{" <<"\"travelTime\":\""<< (it->second.linkTravelTime_)/(it->second.personCnt) <<"\"})" <<std::endl);
		}
	}
}

void Conflux::resetLinkTravelTimes(timeslice frameNumber)
{
	linkTravelTimesMap.clear();
}

void Conflux::incrementSegmentFlow(const RoadSegment* rdSeg, uint16_t statsNum)
{
	SegmentStats* segStats = findSegStats(rdSeg, statsNum);
	segStats->incrementSegFlow();
}

void Conflux::resetSegmentFlows()
{
	for (UpstreamSegmentStatsMap::iterator upstreamIt = upstreamSegStatsMap.begin(); upstreamIt != upstreamSegStatsMap.end(); upstreamIt++)
	{
		const SegmentStatsList& linkSegments = upstreamIt->second;
		for (SegmentStatsList::const_iterator segIt = linkSegments.begin(); segIt != linkSegments.end(); segIt++)
		{
			(*segIt)->resetSegFlow();
		}
	}
}

void Conflux::updateBusStopAgents()
{
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		for (std::vector<SegmentStats*>::const_iterator segStatsIt = upStrmSegMapIt->second.begin(); segStatsIt != upStrmSegMapIt->second.end(); segStatsIt++)
		{
			(*segStatsIt)->updateBusStopAgents(currFrame);
		}
	}
}

void Conflux::assignPersonToBusStopAgent(Person_MT* person)
{
	Role<Person_MT>* role = person->getRole();
	if (role && role->roleType == Role<Person_MT>::RL_WAITBUSACTITITY)
	{
		const BusStop* stop = nullptr;
		if (person->originNode.type_ == WayPoint::BUS_STOP)
		{
			stop = person->originNode.busStop_;
		}

		if (!stop)
		{
			if (person->currSubTrip->origin.type_ == WayPoint::BUS_STOP)
			{
				stop = person->currSubTrip->origin.busStop_;
			}
		}

		if (!stop)
		{
			return;
		}

		//always make sure we dispatch this person only to SOURCE_TERMINUS or NOT_A_TERMINUS stops
		if (stop->terminusType == BusStop::SINK_TERMINUS)
		{
			stop = stop->getTwinStop();
			if (stop->terminusType == BusStop::SINK_TERMINUS)
			{
				throw std::runtime_error("both twin stops are SINKs");
			} //sanity check
		}

		const StreetDirectory& strDirectory = StreetDirectory::instance();
		Agent* busStopAgent = strDirectory.findBusStopAgentByBusStop(stop);
		if (busStopAgent)
		{
			messaging::MessageBus::SendMessage(busStopAgent, MSG_WAITING_PERSON_ARRIVAL, messaging::MessageBus::MessagePtr(new ArrivalAtStopMessage(person)));
		}
	}
}

void Conflux::assignPersonToMRT(Person_MT* person)
{
	Role<Person_MT>* role = person->getRole();
	if (role && role->roleType == Role<Person_MT>::RL_TRAINPASSENGER)
	{
		person->currWorkerProvider = parentWorker;
		messaging::MessageBus::ReRegisterHandler(person, GetContext());
		mrt.push_back(person);
		DailyTime time = person->currSubTrip->endTime;
		person->getRole()->setTravelTime(time.getValue());
		unsigned int tick = ConfigManager::GetInstance().FullConfig().baseGranMS();
		messaging::MessageBus::PostMessage(this, MSG_WAKE_UP, messaging::MessageBus::MessagePtr(new PersonMessage(person)), false, time.getValue() / tick);
	}
}

void Conflux::assignPersonToCar(Person_MT* person)
{
	Role<Person_MT>* role = person->getRole();
	if (role && role->roleType == Role<Person_MT>::RL_CARPASSENGER)
	{
		person->currWorkerProvider = parentWorker;
		PersonList::iterator pIt = std::find(carSharing.begin(), carSharing.end(), person);
		if (pIt == carSharing.end())
		{
			carSharing.push_back(person);
		}
		DailyTime time = person->currSubTrip->endTime;
		person->setStartTime(currFrame.ms());
		person->getRole()->setTravelTime(time.getValue());
		unsigned int tick = ConfigManager::GetInstance().FullConfig().baseGranMS();
		messaging::MessageBus::PostMessage(this, MSG_WAKEUP_CAR_PASSENGER_TELEPORTATION, messaging::MessageBus::MessagePtr(new PersonMessage(person)), false,
				time.getValue() / tick);
	}
}

UpdateStatus Conflux::movePerson(timeslice now, Person_MT* person)
{
	// We give the Agent the benefit of the doubt here and simply call frame_init().
	// This allows them to override the start_time if it seems appropriate (e.g., if they
	// are swapping trip chains). If frame_init() returns false, immediately exit.
	if (!person->isInitialized())
	{
		//Call frame_init() and exit early if required.
		if (!callMovementFrameInit(now, person))
		{
			return UpdateStatus::Done;
		}

		//Set call_frame_init to false here; you can only reset frame_init() in frame_tick()
		person->setInitialized(true); //Only initialize once.
	}

	//Perform the main update tick
	UpdateStatus retVal = callMovementFrameTick(now, person);

	//This persons next movement will be in the next tick
	if (retVal.status != UpdateStatus::RS_DONE && person->remainingTimeThisTick <= 0)
	{
		//now is the right time to ask for resetting of updateParams
		person->setResetParamsRequired(true);
	}

	return retVal;
}

bool GreaterRemainingTimeThisTick::operator ()(const Person_MT* x, const Person_MT* y) const
{
	if ((!x) || (!y))
	{
		std::stringstream debugMsgs;
		debugMsgs << "cmp_person_remainingTimeThisTick: Comparison failed because at least one of the arguments is null" << "|x: " << (x ? x->getId() : 0)
				<< "|y: " << (y ? y->getId() : 0);
		throw std::runtime_error(debugMsgs.str());
	}
	//We want greater remaining time in this tick to translate into a higher priority.
	return (x->getRemainingTimeThisTick() > y->getRemainingTimeThisTick());
}

void sortPersonsDecreasingRemTime(std::deque<Person_MT*>& personList)
{
	GreaterRemainingTimeThisTick greaterRemainingTimeThisTick;
	if (personList.size() > 1)
	{ //ordering is required only if we have more than 1 person in the deque
		std::sort(personList.begin(), personList.end(), greaterRemainingTimeThisTick);
	}
}

std::deque<Person_MT*> Conflux::getAllPersons()
{
	PersonList allPersonsInCfx, tmpAgents;
	SegmentStats* segStats = nullptr;
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		const SegmentStatsList& upstreamSegments = upStrmSegMapIt->second;
		for (SegmentStatsList::const_iterator rdSegIt = upstreamSegments.begin(); rdSegIt != upstreamSegments.end(); rdSegIt++)
		{
			segStats = (*rdSegIt);
			segStats->getPersons(tmpAgents);
			allPersonsInCfx.insert(allPersonsInCfx.end(), tmpAgents.begin(), tmpAgents.end());
		}
	}

	for (VirtualQueueMap::iterator vqMapIt = virtualQueuesMap.begin(); vqMapIt != virtualQueuesMap.end(); vqMapIt++)
	{
		tmpAgents = vqMapIt->second;
		allPersonsInCfx.insert(allPersonsInCfx.end(), tmpAgents.begin(), tmpAgents.end());
	}
	allPersonsInCfx.insert(allPersonsInCfx.end(), activityPerformers.begin(), activityPerformers.end());
	allPersonsInCfx.insert(allPersonsInCfx.end(), pedestrianList.begin(), pedestrianList.end());
	return allPersonsInCfx;
}

unsigned int Conflux::countPersons()
{
	unsigned int numPersons = 0;
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		const SegmentStatsList& upstreamSegments = upStrmSegMapIt->second;
		for (SegmentStatsList::const_iterator statsIt = upstreamSegments.begin(); statsIt != upstreamSegments.end(); statsIt++)
		{
			numPersons = numPersons + (*statsIt)->getNumPersons();
		}
	}
	return numPersons;
}

void Conflux::getAllPersonsUsingTopCMerge(std::deque<Person_MT*>& mergedPersonDeque)
{
	SegmentStats* segStats = nullptr;
	std::vector<PersonList> allPersonLists;
	int sumCapacity = 0;

	//need to calculate the time to intersection for each vehicle.
	//basic test-case shows that this calculation is kind of costly.
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		const SegmentStatsList& upstreamSegments = upStrmSegMapIt->second;
		sumCapacity += (int) (ceil((*upstreamSegments.rbegin())->getCapacity()));
		double totalTimeToSegEnd = 0;
		std::deque<Person_MT*> oneDeque;
		for (SegmentStatsList::const_reverse_iterator rdSegIt = upstreamSegments.rbegin(); rdSegIt != upstreamSegments.rend(); rdSegIt++)
		{
			segStats = (*rdSegIt);
			double speed = segStats->getSegSpeed(true);
			//If speed is 0, treat it as a very small value
			if (speed < INFINITESIMAL_DOUBLE)
			{
				speed = INFINITESIMAL_DOUBLE;
			}
			segStats->updateLinkDrivingTimes(totalTimeToSegEnd);
			PersonList tmpAgents;
			segStats->topCMergeLanesInSegment(tmpAgents);
			totalTimeToSegEnd += segStats->getLength() / speed;
			oneDeque.insert(oneDeque.end(), tmpAgents.begin(), tmpAgents.end());
		}
		allPersonLists.push_back(oneDeque);
	}

	topCMergeDifferentLinksInConflux(mergedPersonDeque, allPersonLists, sumCapacity);
}

void Conflux::topCMergeDifferentLinksInConflux(std::deque<Person_MT*>& mergedPersonDeque, std::vector<std::deque<Person_MT*> >& allPersonLists, int capacity)
{
	std::vector<std::deque<Person_MT*>::iterator> iteratorLists;

	//init location
	size_t dequeSize = allPersonLists.size();
	for (std::vector<std::deque<Person_MT*> >::iterator it = allPersonLists.begin(); it != allPersonLists.end(); ++it)
	{
		iteratorLists.push_back(((*it)).begin());
	}

	//pick the Top C
	for (size_t c = 0; c < capacity; c++)
	{
		double minVal = MAX_DOUBLE;
		Person_MT* currPerson = nullptr;
		std::vector<std::pair<int, Person_MT*> > equiTimeList;
		for (size_t i = 0; i < dequeSize; i++)
		{
			if (iteratorLists[i] != (allPersonLists[i]).end())
			{
				currPerson = (*(iteratorLists[i]));
				if (currPerson->drivingTimeToEndOfLink == minVal)
				{
					equiTimeList.push_back(std::make_pair(i, currPerson));
				}
				else if (currPerson->drivingTimeToEndOfLink < minVal)
				{
					minVal = (*iteratorLists[i])->drivingTimeToEndOfLink;
					equiTimeList.clear();
					equiTimeList.push_back(std::make_pair(i, currPerson));
				}
			}
		}

		if (equiTimeList.empty())
		{
			return; //no more vehicles
		}
		else
		{
			//we have to randomly choose from persons in equiDistantList
			size_t numElements = equiTimeList.size();
			std::pair<int, Person_MT*> chosenPair;
			if (numElements == 1)
			{
				chosenPair = equiTimeList.front();
			}
			else
			{
				int chosenIdx = rand() % numElements;
				chosenPair = equiTimeList[chosenIdx];
			}
			iteratorLists.at(chosenPair.first)++;mergedPersonDeque
			.push_back(chosenPair.second);
		}
	}

	//After pick the Top C, there are still some vehicles left in the deque
	for (size_t i = 0; i < dequeSize; i++)
	{
		if (iteratorLists[i] != (allPersonLists[i]).end())
		{
			mergedPersonDeque.insert(mergedPersonDeque.end(), iteratorLists[i], (allPersonLists[i]).end());
		}
	}
}
//
//void Conflux::addSegTT(Agent::RdSegTravelStat & stats, Person_MT* person) {
//
//	TravelTimeManager::TR &timeRange = TravelTimeManager::getTimeInterval(stats.entryTime);
//	std::map<TravelTimeManager::TR,TravelTimeManager::TT>::iterator itTT = rdSegTravelTimesMap.find(timeRange);
//	TravelTimeManager::TT & travelTimeInfo = (itTT == rdSegTravelTimesMap.end() ? rdSegTravelTimesMap[timeRange] : itTT->second);
//	//initialization just in case
//	if(itTT == rdSegTravelTimesMap.end()){
//		travelTimeInfo[stats.rs].first = 0.0;
//		travelTimeInfo[stats.rs].second = 0;
//	}
//	travelTimeInfo[stats.rs].first += stats.travelTime; //add to total travel time
//	rdSegTravelTimesMap[timeRange][stats.rs].second ++; //increment the total contribution
//}
//
//void Conflux::resetRdSegTravelTimes() {
//	rdSegTravelTimesMap.clear();
//}
//
//void Conflux::reportRdSegTravelTimes(timeslice frameNumber) {
//	if (ConfigManager::GetInstance().CMakeConfig().OutputEnabled()) {
//		std::map<const RoadSegment*, RdSegTravelTimes>::const_iterator it = rdSegTravelTimesMap.begin();
//		for( ; it != rdSegTravelTimesMap.end(); ++it ) {
//			LogOut("(\"rdSegTravelTime\""
//				<<","<<frameNumber.frame()
//				<<","<<it->first
//				<<",{"
//				<<"\"travelTime\":\""<< (it->second.travelTimeSum)/(it->second.agCnt)
//				<<"\"})"<<std::endl);
//		}
//	}
////	if (ConfigManager::GetInstance().FullConfig().PathSetMode()) {
////		insertTravelTime2TmpTable(frameNumber, rdSegTravelTimesMap);
////	}
//}
//
//bool Conflux::insertTravelTime2TmpTable(timeslice frameNumber, std::map<const RoadSegment*, Conflux::RdSegTravelTimes>& rdSegTravelTimesMap)
//{
////	bool res=false;
////	//Link_travel_time& data
////	std::map<const RoadSegment*, Conflux::RdSegTravelTimes>::const_iterator it = rdSegTravelTimesMap.begin();
////	for (; it != rdSegTravelTimesMap.end(); it++){
////		LinkTravelTime tt;
////		const DailyTime &simStart = ConfigManager::GetInstance().FullConfig().simStartTime();
////		tt.linkId = (*it).first->getId();
////		tt.recordTime_DT = simStart + DailyTime(frameNumber.ms());
////		tt.travelTime = (*it).second.travelTimeSum/(*it).second.agCnt;
////		PathSetManager::getInstance()->insertTravelTime2TmpTable(tt);
////	}
////	return res;
//}

void Conflux::findBoundaryConfluxes()
{

	Worker* firstUpstreamWorker = nullptr;
	std::map<const MultiNode*, Conflux*>& multinode_confluxes = MT_Config::getInstance().getConfluxNodes();

	for (UpstreamSegmentStatsMap::iterator i = upstreamSegStatsMap.begin(); i != upstreamSegStatsMap.end(); i++)
	{
		const MultiNode* upnode = dynamic_cast<const MultiNode*>(i->first->getStart());

		if (upnode)
		{
			std::map<const MultiNode*, Conflux*>::iterator confIt = multinode_confluxes.find(upnode);
			if (confIt != multinode_confluxes.end())
			{
				//check if upstream conflux belongs to another worker
				if (confIt->second->getParentWorker() != this->getParentWorker())
				{
					if (!isBoundary)
					{
						isBoundary = true;
						firstUpstreamWorker = confIt->second->getParentWorker();
					}
					else if (confIt->second->getParentWorker() != firstUpstreamWorker && firstUpstreamWorker)
					{
						isMultipleReceiver = true;
						return;
					}
				}
			}
		}
	}
}

unsigned int Conflux::getNumRemainingInLaneInfinity()
{
	unsigned int count = 0;
	SegmentStats* segStats = nullptr;
	for (UpstreamSegmentStatsMap::iterator upStrmSegMapIt = upstreamSegStatsMap.begin(); upStrmSegMapIt != upstreamSegStatsMap.end(); upStrmSegMapIt++)
	{
		const SegmentStatsList& segStatsList = upStrmSegMapIt->second;
		for (SegmentStatsList::const_iterator statsIt = segStatsList.begin(); statsIt != segStatsList.end(); statsIt++)
		{
			segStats = (*statsIt);
			count += segStats->numAgentsInLane(segStats->laneInfinity);
		}
	}
	return count;
}

Conflux* Conflux::findStartingConflux(Person_MT* person, unsigned int now)
{
	UpdateStatus res = person->checkTripChain();
	if (res.status == UpdateStatus::RS_DONE)
	{
		return nullptr;
	} //person without trip chain will be thrown out of the simulation
	person->setStartTime(now);

	Role<Person_MT>* personRole = person->getRole();
	if (!personRole)
	{
		return nullptr;
	}
	if ((*person->currTripChainItem)->itemType == TripChainItem::IT_ACTIVITY)
	{
		//IT_ACTIVITY as of now is just a matter of waiting for a period of time(between its start and end time)
		//since start time of the activity is usually later than what is configured initially,
		//we have to make adjustments so that it waits for exact amount of time
		ActivityPerformer<Person_MT>* ap = dynamic_cast<ActivityPerformer<Person_MT>*>(personRole);
		ap->setActivityStartTime(DailyTime(now + ConfigManager::GetInstance().FullConfig().baseGranMS()));
		ap->setActivityEndTime(
				DailyTime(
						now + ConfigManager::GetInstance().FullConfig().baseGranMS()
								+ ((*person->currTripChainItem)->endTime.getValue() - (*person->currTripChainItem)->startTime.getValue())));

		const MultiNode* activityLocation = dynamic_cast<MultiNode*>(ap->getLocation());
		if (activityLocation) //activity locations must ideally be multinodes
		{
			return MT_Config::getInstance().getConfluxForNode(activityLocation);
		}
		return nullptr;
	}

	//Now that the Role<Person_MT> has been fully constructed, initialize it.
	personRole->Movement()->frame_init();
	if (person->isToBeRemoved())
	{
		return nullptr;
	} //if agent initialization fails, person is set to be removed
	person->setInitialized(true);

	return personRole->Movement()->getStartingConflux();
}

void Conflux::insertIncident(SegmentStats* segStats, double newFlowRate)
{
	const std::vector<Lane*>& lanes = segStats->getRoadSegment()->getLanes();
	for (std::vector<Lane*>::const_iterator it = lanes.begin(); it != lanes.end(); it++)
	{
		segStats->updateLaneParams((*it), newFlowRate);
	}
}

void Conflux::removeIncident(SegmentStats* segStats)
{
	const std::vector<Lane*>& lanes = segStats->getRoadSegment()->getLanes();
	for (std::vector<Lane*>::const_iterator it = lanes.begin(); it != lanes.end(); it++)
	{
		segStats->restoreLaneParams(*it);
	}
}

InsertIncidentMessage::InsertIncidentMessage(const std::vector<SegmentStats*>& stats, double newFlowRate) :
		stats(stats), newFlowRate(newFlowRate)
{
}

InsertIncidentMessage::~InsertIncidentMessage()
{
}

PersonMessage::PersonMessage(Person_MT* inPerson) :
		person(inPerson)
{
}

PersonMessage::~PersonMessage()
{
}

ArrivalAtStopMessage::ArrivalAtStopMessage(Person_MT* person) :
		waitingPerson(person)
{
}

ArrivalAtStopMessage::~ArrivalAtStopMessage()
{
}
