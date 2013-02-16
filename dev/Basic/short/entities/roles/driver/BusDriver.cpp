/* Copyright Singapore-MIT Alliance for Research and Technology */

#include "BusDriver.hpp"

#include <vector>
#include <iostream>
#include <cmath>
#include "DriverUpdateParams.hpp"

#include "entities/Person.hpp"
#include "entities/vehicle/BusRoute.hpp"
#include "entities/vehicle/Bus.hpp"
#include "entities/BusController.hpp"
#include "entities/roles/passenger/Passenger.hpp"

#include "geospatial/Point2D.hpp"
#include "geospatial/BusStop.hpp"
#include "geospatial/RoadSegment.hpp"
#include "geospatial/aimsun/Loader.hpp"
#include "partitions/PackageUtils.hpp"
#include "partitions/UnPackageUtils.hpp"
#include "entities/AuraManager.hpp"
#include "util/PassengerDistribution.hpp"

using namespace sim_mob;
using std::vector;
using std::map;
using std::string;

namespace {
//const int BUS_STOP_WAIT_PASSENGER_TIME_SEC = 2;
}//End anonymous namespace


sim_mob::BusDriver::BusDriver(Person* parent, MutexStrategy mtxStrat) :Driver(parent, mtxStrat), nextStop(nullptr), waitAtStopMS(-1), lastTickDistanceToBusStop(-1), lastVisited_BusStop(mtxStrat, nullptr), lastVisited_BusStopSequenceNum(mtxStrat, 0), real_DepartureTime(mtxStrat, 0), real_ArrivalTime(mtxStrat, 0), DwellTime_ijk(mtxStrat, 0), busstop_sequence_no(mtxStrat, 0), first_busstop(true), last_busstop(false), no_passengers_boarding(0), no_passengers_alighting(0)
{
	BUS_STOP_WAIT_PASSENGER_TIME_SEC = 2;
	dwellTime_record = 0;
	passengerCountOld_display_flag = false;
	curr_busStopRealTimes = new Shared<BusStop_RealTimes>(mtxStrat,BusStop_RealTimes());
	xpos_approachingbusstop=-1;
	ypos_approachingbusstop=-1;
	demo_passenger_increase = false;

	if(parent) {
		if(parent->getAgentSrc() == "BusController") {
			BusTrip* bustrip = dynamic_cast<BusTrip*>(*(parent->currTripChainItem));
			if(bustrip && bustrip->itemType==TripChainItem::IT_BUSTRIP) {
				std::vector<const BusStop*> busStops_temp = bustrip->getBusRouteInfo().getBusStops();
				std::cout << "busStops_temp.size() " << busStops_temp.size() << std::endl;
				for(int i = 0; i < busStops_temp.size(); i++) {
					Shared<BusStop_RealTimes>* pBusStopRealTimes = new Shared<BusStop_RealTimes>(mtxStrat,BusStop_RealTimes());
					busStopRealTimes_vec_bus.push_back(pBusStopRealTimes);
				}
			}
		}
	}
}

Role* sim_mob::BusDriver::clone(Person* parent) const {
	return new BusDriver(parent, parent->getMutexStrategy());
}

Vehicle* sim_mob::BusDriver::initializePath_bus(bool allocateVehicle) {
	Vehicle* res = nullptr;

	//Only initialize if the next path has not been planned for yet.
	if (!parent->getNextPathPlanned()) {
		vector<const RoadSegment*> path;
		Person* person = dynamic_cast<Person*>(parent);
		int vehicle_id = 0;
		int laneID = -1;
		if (person) {
			const BusTrip* bustrip =dynamic_cast<const BusTrip*>(*(person->currTripChainItem));
			if (!bustrip)
				std::cout << "bustrip is null\n";
			if (bustrip&& (*(person->currTripChainItem))->itemType== TripChainItem::IT_BUSTRIP) {
				path = bustrip->getBusRouteInfo().getRoadSegments();
				std::cout << "BusTrip path size = " << path.size() << std::endl;
				vehicle_id = bustrip->getVehicleID();
				if (path.size() > 0) {
					laneID = path.at(0)->getLanes().size() - 2;
				}
			} else {
				if ((*(person->currTripChainItem))->itemType== TripChainItem::IT_TRIP)
					std::cout << TripChainItem::IT_TRIP << " IT_TRIP\n";
				if ((*(person->currTripChainItem))->itemType== TripChainItem::IT_ACTIVITY)
					std::cout << "IT_ACTIVITY\n";
				if ((*(person->currTripChainItem))->itemType== TripChainItem::IT_BUSTRIP)
			       std::cout << "IT_BUSTRIP\n";
				std::cout<< "BusTrip path not initialized coz it is not a bustrip, (*(person->currTripChainItem))->itemType = "<< (*(person->currTripChainItem))->itemType<< std::endl;
			}

		}

		//TODO: Start in lane 0?
		int startlaneID = 1;

		BusDriver* v = dynamic_cast<BusDriver*>(this);
		if (v && laneID != -1) {
			startlaneID = laneID; //need to check if lane valid
			//parentP->laneID = -1;
		}

		// Bus should be at least 1200 to be displayed on Visualizer
		const double length = dynamic_cast<BusDriver*>(this) ? 1200 : 400;
		const double width = 200;

		//A non-null vehicle means we are moving.
		if (allocateVehicle) {
			res = new Vehicle(path, startlaneID, vehicle_id, length, width);
		}
	}

	//to indicate that the path to next activity is already planned
	parent->setNextPathPlanned(true);
	return res;
}

//We're recreating the parent class's frame_init() method here.
// Our goal is to reuse as much of Driver as possible, and then
// refactor common code out later. We could just call the frame_init() method
// directly, but there's some unexpected interdependencies.
void sim_mob::BusDriver::frame_init(UpdateParams& p) {
	//TODO: "initializePath()" in Driver mixes initialization of the path and
	//      creation of the Vehicle (e.g., its width/height). These are both
	//      very different for Cars and Buses, but until we un-tangle the code
	//      we'll need to rely on hacks like this.
	Vehicle* newVeh = nullptr;
	Person* person = dynamic_cast<Person*>(parent);
	if (person) {
		if (person->getAgentSrc() == "BusController") {
			newVeh = initializePath_bus(true); // no need any node information
		} else {
			newVeh = initializePath(true); // previous node to node calculation
		}
	}

	//Save the path, create a vehicle.
	if (newVeh) {
		//Use this sample vehicle to build our Bus, then delete the old vehicle.
		BusRoute nullRoute; //Buses don't use the route at the moment.

		TripChainItem* tci = *person->currTripChainItem;
		BusTrip* bustrip_change =dynamic_cast<BusTrip*>(tci);
		if (!bustrip_change) {
			throw std::runtime_error("BusDriver created without an appropriate BusTrip item.");
		}

		vehicle = new Bus(nullRoute, newVeh,bustrip_change->getBusline()->getBusLineID());
		delete newVeh;

		//This code is used by Driver to set a few properties of the Vehicle/Bus.
		if (!vehicle->hasPath()) {
			throw std::runtime_error(
					"Vehicle could not be created for bus driver; no route!");
		}

		//Set the bus's origin and set of stops.
		setOrigin(params);
		if (person) {
			if (person->getAgentSrc() == "BusController") {
				const BusTrip* bustrip =dynamic_cast<const BusTrip*>(*(person->currTripChainItem));
				if (bustrip && bustrip->itemType == TripChainItem::IT_BUSTRIP) {
					busStops = bustrip->getBusRouteInfo().getBusStops();
					if (busStops.empty()) {
						std::cout<< "Error: No BusStops assigned from BusTrips!!! "<< std::endl;
						// This case can be true, so use the BusStops found by Path instead
						busStops = findBusStopInPath(vehicle->getCompletePath());
					}
				}
			} else {
				busStops = findBusStopInPath(vehicle->getCompletePath());
			}
		}
		//Unique to BusDrivers: reset your route
		waitAtStopMS = 0.0;
	}
}

vector<const BusStop*> sim_mob::BusDriver::findBusStopInPath(const vector<const RoadSegment*>& path) const {
	//NOTE: Use typedefs instead of defines.
	typedef vector<const BusStop*> BusStopVector;
	BusStopVector res;
	int busStopAmount = 0;
	vector<const RoadSegment*>::const_iterator it;
	for (it = path.begin(); it != path.end(); ++it) {
		// get obstacles in road segment
		const RoadSegment* rs = (*it);
		const std::map<centimeter_t, const RoadItem*> & obstacles =rs->obstacles;

		//Check each of these.
		std::map<centimeter_t, const RoadItem*>::const_iterator ob_it;
		for (ob_it = obstacles.begin(); ob_it != obstacles.end(); ++ob_it) {
			RoadItem* ri = const_cast<RoadItem*>(ob_it->second);
			BusStop *bs = dynamic_cast<BusStop*>(ri);
			if (bs) {
				res.push_back(bs);
			}
		}
	}
	return res;
}
double sim_mob::BusDriver::linkDriving(DriverUpdateParams& p)
{
	if ((params.now.ms() / 1000.0 - startTime > 10)&& vehicle->getDistanceMovedInSegment() > 2000 && isAleadyStarted == false) {
		isAleadyStarted = true;
	}
	p.isAlreadyStart = isAleadyStarted;
	if (!vehicle->hasNextSegment(true)) {
		p.dis2stop = vehicle->getAllRestRoadSegmentsLength()- vehicle->getDistanceMovedInSegment() - vehicle->length / 2- 300;
		if (p.nvFwd.distance < p.dis2stop)
			p.dis2stop = p.nvFwd.distance;
		p.dis2stop /= 100;
	} else {
		p.nextLaneIndex = std::min<int>(p.currLaneIndex,vehicle->getNextSegment()->getLanes().size() - 1);
		if (vehicle->getNextSegment()->getLanes().at(p.nextLaneIndex)->is_pedestrian_lane()) {
			p.nextLaneIndex--;
			p.dis2stop = vehicle->getCurrPolylineLength()- vehicle->getDistanceMovedInSegment() + 1000;
		} else
			p.dis2stop = 1000;//defalut 1000m
	}

	//get nearest car, if not making lane changing, the nearest car should be the leading car in current lane.
	//if making lane changing, adjacent car need to be taken into account.
	NearestVehicle & nv = nearestVehicle(p);
//	if (p.nvFwd.exists())
//		p.space = p.nvFwd.distance;
//	else
//		p.space = 50;
//	if (nv.distance <= 0) {
//		//if (nv.driver->parent->getId() > this->parent->getId())
//		if (getDriverParent(nv.driver)->getId() > this->parent->getId()) {
//			nv = NearestVehicle();
//		}
//	}
	if (isAleadyStarted == false) {
		if (nv.distance <= 0) {
			if (getDriverParent(nv.driver)->getId() > this->parent->getId()) {
				nv = NearestVehicle();
			}

		}
	}
	//this function make the issue Ticket #86
	perceivedDataProcess(nv, p);
	Person* person = dynamic_cast<Person*>(parent);
	const BusTrip* bustrip =dynamic_cast<const BusTrip*>(*(person->currTripChainItem));

	//bus approaching bus stop reduce speed
	//and if its left has lane, merge to left lane
	//if (isBusFarawayBusStop()) {
//	double acci = busAccelerating(p)*100;
//	vehicle->setAcceleration(acci);
	p.currSpeed = vehicle->getVelocity() / 100;
	double newFwdAcc = 0;
	newFwdAcc = cfModel->makeAcceleratingDecision(p, targetSpeed, maxLaneSpeed);
	if (abs(vehicle->getTurningDirection() != LCS_SAME) && newFwdAcc > 0&& vehicle->getVelocity() / 100 > 10) {
		newFwdAcc = 0;
	}
	vehicle->setAcceleration(newFwdAcc * 100);
	//}

//	//Retrieve a new acceleration value.
//		double newFwdAcc = 0;
//		//Convert back to m/s
//		//TODO: Is this always m/s? We should rename the variable then...
//		p.currSpeed = vehicle->getVelocity() / 100;
//		//Call our model
//		//Return the remaining amount (obtained by calling updatePositionOnLink)
//		newFwdAcc = cfModel->makeAcceleratingDecision(p, targetSpeed, maxLaneSpeed);
//		//Update our chosen acceleration; update our position on the link.
//		vehicle->setAcceleration(newFwdAcc * 100);

	//NOTE: Driver already has a lcModel; we should be able to just use this. ~Seth
	LANE_CHANGE_SIDE lcs = LCS_SAME;
	MITSIM_LC_Model* mitsim_lc_model = dynamic_cast<MITSIM_LC_Model*>(lcModel);
	if (mitsim_lc_model) {
		lcs = mitsim_lc_model->makeMandatoryLaneChangingDecision(p);
	} else {
		throw std::runtime_error("TODO: BusDrivers currently require the MITSIM lc model.");
	}

	vehicle->setTurningDirection(lcs);
	double newLatVel;
	newLatVel = lcModel->executeLaneChanging(p,vehicle->getAllRestRoadSegmentsLength(), vehicle->length,vehicle->getTurningDirection());
	vehicle->setLatVelocity(newLatVel * 10);
	if (vehicle->getLatVelocity() > 0)
		vehicle->setTurningDirection(LCS_LEFT);
	else if (vehicle->getLatVelocity() < 0)
		vehicle->setTurningDirection(LCS_RIGHT);
	else
		vehicle->setTurningDirection(LCS_SAME);

	p.turningDirection = vehicle->getTurningDirection();

	if (isBusApproachingBusStop()) {
		double acc = busAccelerating(p) * 100;

		//move to most left lane
		p.nextLaneIndex =vehicle->getCurrSegment()->getLanes().back()->getLaneID();
		LANE_CHANGE_SIDE lcs =mitsim_lc_model->makeMandatoryLaneChangingDecision(p);
		vehicle->setTurningDirection(lcs);
		double newLatVel;
		newLatVel = mitsim_lc_model->executeLaneChanging(p,vehicle->getAllRestRoadSegmentsLength(), vehicle->length,vehicle->getTurningDirection());
		vehicle->setLatVelocity(newLatVel * 5);

		// reduce speed
		if (vehicle->getVelocity() / 100.0 > 2.0) {
			if (acc < -500.0) {
				vehicle->setAcceleration(acc);
			} else
				vehicle->setAcceleration(-500);
		}
		Person* person = dynamic_cast<Person*>(parent);
		const BusTrip* bustrip =dynamic_cast<const BusTrip*>(*(person->currTripChainItem));
		waitAtStopMS = 0;
	}


	if (isBusArriveBusStop() && (waitAtStopMS >= 0)&& (waitAtStopMS < BUS_STOP_WAIT_PASSENGER_TIME_SEC)) {

//		if ((vehicle->getVelocity()/100) > 0)
			vehicle->setAcceleration(-5000);
		if (vehicle->getVelocity()/100 < 1)
			vehicle->setVelocity(0);

		if ((vehicle->getVelocity()/100 < 0.1) && (waitAtStopMS < BUS_STOP_WAIT_PASSENGER_TIME_SEC)) {
			waitAtStopMS = waitAtStopMS + p.elapsedSeconds;

			//Pick up a semi-random number of passengers
			Bus* bus = dynamic_cast<Bus*>(vehicle);
			if ((waitAtStopMS == p.elapsedSeconds) && bus)
			{
				std::cout << "real_ArrivalTime value: " << real_ArrivalTime.get() << "  DwellTime_ijk: " << DwellTime_ijk.get() << std::endl;
				real_ArrivalTime.set(p.now.ms());// BusDriver set RealArrival Time, set once(the first time comes in)
				bus->TimeOfBusreachingBusstop=p.now.ms();

				//From Meenu's branch; enable if needed.
				//dwellTime_record = passengerGeneration(bus);//if random distribution is to be used,uncomment

				dwellTime_record = passengerGenerationNew(bus);
				//Back to both branches:
				DwellTime_ijk.set(dwellTime_record);
			}
			if ((waitAtStopMS == p.elapsedSeconds * 2.0) && bus) {
				// 0.2sec, return and reset BUS_STOP_WAIT_PASSENGER_TIME_SEC
				// (no control: use dwellTime;
				// has Control: use DwellTime to calculate the holding strategy and return BUS_STOP_WAIT_PASSENGER_TIME_SEC
				if(BusController::HasBusControllers()) {
					Person* person = dynamic_cast<Person*>(parent);
					if(person) {
						BusTrip* bustrip = const_cast<BusTrip*>(dynamic_cast<const BusTrip*>(*(person->currTripChainItem)));
						if(bustrip && bustrip->itemType==TripChainItem::IT_BUSTRIP) {
							const Busline* busline = bustrip->getBusline();
							if (busline) {
								if(busline->getControl_TimePointNum0() == busstop_sequence_no.get() || busline->getControl_TimePointNum1() == busstop_sequence_no.get()) { // only use holding control at selected time points
									double waitTime = 0;
									waitTime = BusController::TEMP_Get_Bc_1()->decisionCalculation(busline->getBusLineID(),bustrip->getBusTripRun_SequenceNum(),busstop_sequence_no.get(),real_ArrivalTime.get(),DwellTime_ijk.get(),getBusStop_RealTimes(),lastVisited_BusStop.get());
									setWaitTime_BusStop(waitTime);
								} else { // other bus stops store the real time values
									setWaitTime_BusStop(DwellTime_ijk.get());// ignore the other BusStops, just use DwellTime
									BusController::TEMP_Get_Bc_1()->storeRealTimes_eachBusStop(busline->getBusLineID(),bustrip->getBusTripRun_SequenceNum(),busstop_sequence_no.get(),real_ArrivalTime.get(),DwellTime_ijk.get(),lastVisited_BusStop.get(),getBusStop_RealTimes());
								}
								bustrip->lastVisitedStop_SequenceNumber = busstop_sequence_no.get();
							} else {
								std::cout << "Busline is nullptr, something is wrong!!! " << std::endl;
								setWaitTime_BusStop(DwellTime_ijk.get());
							}
						}
					}
				} else {
					setWaitTime_BusStop(DwellTime_ijk.get());
				}
			}
			if (waitAtStopMS >= dwellTime_record) {
				passengerCountOld_display_flag = false;
			} else {
				passengerCountOld_display_flag = true;
			}
		}
	}
	if (isBusLeavingBusStop()
			|| (waitAtStopMS >= BUS_STOP_WAIT_PASSENGER_TIME_SEC)) {
		std::cout << "BusDriver::updatePositionOnLink: bus isBusLeavingBusStop"
				<< std::endl;
		waitAtStopMS = -1;
		BUS_STOP_WAIT_PASSENGER_TIME_SEC = 2;// reset when leaving bus stop
		vehicle->setAcceleration(busAccelerating(p) * 100);
	}

	//Update our distance
	lastTickDistanceToBusStop = distanceToNextBusStop();

	DynamicVector segmentlength(
			vehicle->getCurrSegment()->getStart()->location.getX(),
			vehicle->getCurrSegment()->getStart()->location.getY(),
			vehicle->getCurrSegment()->getEnd()->location.getX(),
			vehicle->getCurrSegment()->getEnd()->location.getY());

	//Return the remaining amount (obtained by calling updatePositionOnLink)
	return updatePositionOnLink(p);
}

double sim_mob::BusDriver::getPositionX() const {
	return vehicle ? vehicle->getX() : 0;
}

double sim_mob::BusDriver::getPositionY() const {
	return vehicle ? vehicle->getY() : 0;
}

double sim_mob::BusDriver::busAccelerating(DriverUpdateParams& p) {
	//Retrieve a new acceleration value.
	double newFwdAcc = 0;

	//Convert back to m/s
	//TODO: Is this always m/s? We should rename the variable then...
	p.currSpeed = vehicle->getVelocity() / 100;

	//Call our model
	newFwdAcc = cfModel->makeAcceleratingDecision(p, targetSpeed, maxLaneSpeed);

	return newFwdAcc;
	//Update our chosen acceleration; update our position on the link.
	//vehicle->setAcceleration(newFwdAcc * 100);
}

bool sim_mob::BusDriver::isBusFarawayBusStop() {
	bool res = false;
	double distance = distanceToNextBusStop();
	if (distance < 0 || distance > 50)
		res = true;

	return res;
}

bool sim_mob::BusDriver::isBusApproachingBusStop() {
	double distance = distanceToNextBusStop();
	if (distance >= 10 && distance <= 50) {
		if (lastTickDistanceToBusStop < 0) {
			return true;
		} else if (lastTickDistanceToBusStop > distance) {
			return true;
		}
	}
	return false;
}

bool sim_mob::BusDriver::isBusArriveBusStop() {
	double distance = distanceToNextBusStop();
	return (distance > 0 && distance < 10);
}

bool sim_mob::BusDriver::isBusGngtoBreakDown() {
	double distance = distanceToNextBusStop();
	return (distance > 10 && distance < 14);
}

bool sim_mob::BusDriver::isBusLeavingBusStop() {
	double distance = distanceToNextBusStop();
	if (distance >= 10 && distance < 50) {
		if (distance < 0) {
			lastTickDistanceToBusStop = distance;
			return true;
		} else if (lastTickDistanceToBusStop < distance) {
			lastTickDistanceToBusStop = distance;
			return true;
		}
	}

	lastTickDistanceToBusStop = distance;
	return false;
}

double sim_mob::BusDriver::distanceToNextBusStop() {
//	if (!vehicle->getCurrSegment() || !vehicle->hasNextSegment(true)) {
//		return -1;
//	}

	double distanceToCurrentSegmentBusStop = getDistanceToBusStopOfSegment(
			vehicle->getCurrSegment());
	double distanceToNextSegmentBusStop = -1;
	if (vehicle->hasNextSegment(true))
		distanceToNextSegmentBusStop = getDistanceToBusStopOfSegment(
				vehicle->getNextSegment(true));

	if (distanceToCurrentSegmentBusStop >= 0
			&& distanceToNextSegmentBusStop >= 0) {
		return ((distanceToCurrentSegmentBusStop <= distanceToNextSegmentBusStop) ?
				distanceToCurrentSegmentBusStop : distanceToNextSegmentBusStop);
	} else if (distanceToCurrentSegmentBusStop > 0) {
		return distanceToCurrentSegmentBusStop;
	}

	return distanceToNextSegmentBusStop;
}

void sim_mob::BusDriver::Board_passengerGeneration(Bus* bus)//for generating random passenger distribution at the bus stop
{
	ConfigParams& config = ConfigParams::GetInstance();
	if (bus) {
		int manualID = -1;
		map<string, string> props;
		props["#mode"] = "BusTravel";
		props["#time"] = "0";
		for (int no = 0; no < no_passengers_boarding; no++) {
			//create passenger objects in the bus,bus has a list of passenger objects
			//Create the Person agent with that given ID (or an auto-generated one)
			Person* agent = new Person("XML_Def", config.mutexStategy,
					manualID);
			agent->setConfigProperties(props);
			agent->setStartTime(0);
			bus->passengers_distribition.push_back(agent);
		}
	}

}

void sim_mob::BusDriver::Alight_passengerGeneration(Bus* bus)//alighting passenger function if random distribution is used
{
	//delete passenger objects in the bus
	if (bus) {
		for (int no = 0; no < no_passengers_alighting; no++) {
			//delete passenger objects from the bus
			bus->passengers_distribition.pop_back();
		}
	}
}

double sim_mob::BusDriver::passengerGenerationNew(Bus* bus)// new function to  passenger boarding/alighting and dwell time calculation
{
	double DTijk = 0.0;
	no_passengers_alighting=0;
	no_passengers_boarding=0;
	bus->setPassengerCountOld(bus->getPassengerCount());// record the old passenger number
	AlightingPassengers(bus);//first alight passengers inside the bus
	BoardingPassengers(bus);//then board passengers waiting at the bus stop
	DTijk = dwellTimeCalculation(no_passengers_alighting,no_passengers_boarding,0,0,0,bus->getPassengerCountOld());
	return DTijk;
}

double sim_mob::BusDriver::passengerGeneration(Bus* bus)//random passenger distribution(not used now)
{
	double DTijk = 0.0;
	size_t no_passengers_bus = 0;
	size_t no_passengers_busstop = 0;
	ConfigParams& config = ConfigParams::GetInstance();
	PassengerDist* passenger_dist = config.passengerDist_busstop;
	//create the passenger objects at the bus stop=random no-boarding passengers
	if (bus && passenger_dist)
	{
		no_passengers_busstop = passenger_dist->getnopassengers();
		no_passengers_bus = bus->getPassengerCount();
		bus->setPassengerCountOld(no_passengers_bus); // record the old passenger number
		//if last busstop,only alighting
		if (last_busstop == true)
		{
			no_passengers_alighting = no_passengers_bus; // last bus stop, all alighting
			no_passengers_boarding = 0; // reset boarding passengers to be zero at the last bus stop(for dwell time)
			Alight_passengerGeneration(bus); //alight the bus
			no_passengers_bus = no_passengers_bus - no_passengers_alighting;
			bus->setPassengerCount(no_passengers_bus);
			last_busstop = false;
		}
		//if first busstop,only boarding
		else if(first_busstop ==true)
		{
			no_passengers_boarding = (config.percent_boarding * 0.01)*no_passengers_busstop;
			if(no_passengers_boarding > bus->getBusCapacity() - no_passengers_bus)
				no_passengers_boarding = bus->getBusCapacity() - no_passengers_bus;
			Board_passengerGeneration(bus);//board the bus
			bus->setPassengerCount(no_passengers_bus+no_passengers_boarding);
			first_busstop= false;
		}

		//normal busstop,both boarding and alighting
		else {
			no_passengers_alighting = (config.percent_alighting * 0.01)* no_passengers_bus;
			Alight_passengerGeneration(bus); //alight the bus
			no_passengers_bus = no_passengers_bus - no_passengers_alighting;
			bus->setPassengerCount(no_passengers_bus);
			no_passengers_boarding = (config.percent_boarding * 0.01)* no_passengers_busstop;
			if (no_passengers_boarding> bus->getBusCapacity() - no_passengers_bus)
				no_passengers_boarding = bus->getBusCapacity()- no_passengers_bus;
			Board_passengerGeneration(bus); //board the bus
			bus->setPassengerCount(no_passengers_bus + no_passengers_boarding);
		}
		DTijk = dwellTimeCalculation(no_passengers_alighting,no_passengers_boarding, 0, 0, 0, bus->getPassengerCount());
		return DTijk;
	} else {
		throw std::runtime_error("Passenger distributions have not been initialized yet.");
		return -1;
	}
}

double sim_mob::BusDriver::dwellTimeCalculation(int A, int B, int delta_bay, int delta_full,int Pfront, int no_of_passengers)
{
	//assume single channel passenger movement
	 double alpha1 = 2.1;//alighting passenger service time,assuming payment by smart card
	 double alpha2 = 3.5;//boarding passenger service time,assuming alighting through rear door
	 double alpha3 = 3.5;//door opening and closing times
	 double alpha4 = 1.0;
	 double beta1 = 0.7;//fixed parameters
	 double beta2 = 0.7;
	 double beta3 = 5;
	 double DTijk = 0.0;
	 bool bus_crowdness_factor;
	int no_of_seats = 40;
	if (no_of_passengers > no_of_seats) //standees are present
		alpha1 += 0.5; //boarding time increase if standees are present
	if (no_of_passengers > no_of_seats)
		bus_crowdness_factor = 1;
	else
		bus_crowdness_factor = 0;
	double PTijk_front = alpha1 * Pfront * A + alpha2 * B+ alpha3 * bus_crowdness_factor * B;
	double PTijk_rear = alpha4 * (1 - Pfront) * A;
	double PT;
	PT = std::max(PTijk_front, PTijk_rear);
	DTijk = beta1 + PT + beta2 * delta_bay + beta3 * delta_full;
	std::cout<<"Dwell__time "<<DTijk<<std::endl;
	return DTijk;
}

double sim_mob::BusDriver::getDistanceToBusStopOfSegment(const RoadSegment* rs) {

	double distance = -100;
	double currentX = vehicle->getX();
	double currentY = vehicle->getY();
	const std::map<centimeter_t, const RoadItem*> & obstacles = rs->obstacles;
	for (std::map<centimeter_t, const RoadItem*>::const_iterator o_it =
			obstacles.begin(); o_it != obstacles.end(); o_it++) {
		RoadItem* ri = const_cast<RoadItem*>(o_it->second);
		BusStop *bs = dynamic_cast<BusStop *>(ri);
		int stopPoint = o_it->first;

		if (bs) {
			// check bs
			int i = 0;
			//int busstop_sequence_no = 0;
			bool isFound = false;

			for (i = 0; i < busStops.size(); ++i) {
				if (bs->getBusstopno_() == busStops[i]->getBusstopno_()) {
					isFound = true;
					busstop_sequence_no.set(i);
					lastVisited_BusStop.set(busStops[i]);
					break;
				}
			}
			if (isFound) {

				xpos_approachingbusstop = bs->xPos;
				ypos_approachingbusstop = bs->yPos;
				if (busstop_sequence_no.get() == (busStops.size() - 1)) // check whether it is the last bus stop in the busstop list
						{
					last_busstop = true;
				}
				if (rs == vehicle->getCurrSegment()) {

					if (stopPoint < 0) {
						throw std::runtime_error(
								"BusDriver offset in obstacles list should never be <0");
					}

					if (stopPoint >= 0) {
						DynamicVector BusDistfromStart(vehicle->getX(),
								vehicle->getY(),
								rs->getStart()->location.getX(),
								rs->getStart()->location.getY());
						distance = stopPoint
								- vehicle->getDistanceMovedInSegment();
						break;

					}

				} else {
					DynamicVector busToSegmentStartDistance(currentX, currentY,
							rs->getStart()->location.getX(),
							rs->getStart()->location.getY());
					distance = vehicle->getCurrentSegmentLength()
							- vehicle->getDistanceMovedInSegment() + stopPoint;

				}
			} // end of if isFound
		}
	}

	return distance / 100.0;
}

//Main update functionality
void sim_mob::BusDriver::frame_tick(UpdateParams& p) {
	//NOTE: If this is all that is doen, we can simply delete this function and
	//      let its parent handle it automatically. ~Seth
	Driver::frame_tick(p);
}

void sim_mob::BusDriver::frame_tick_output(const UpdateParams& p) {
	//Skip?
	if (vehicle->isDone()
			|| ConfigParams::GetInstance().is_run_on_many_computers) {
		return;
	}

#ifndef SIMMOB_DISABLE_OUTPUT
	double baseAngle =
			vehicle->isInIntersection() ?
					intModel->getCurrentAngle() : vehicle->getAngle();
	Bus* bus = dynamic_cast<Bus*>(vehicle);
	if (!passengerCountOld_display_flag) {
		LogOut(
				"(\"BusDriver\""
				<<","<<p.now.frame()
				<<","<<parent->getId()
				<<",{" <<"\"xPos\":\""<<static_cast<int>(bus->getX())
				<<"\",\"yPos\":\""<<static_cast<int>(bus->getY())
				<<"\",\"angle\":\""<<(360 - (baseAngle * 180 / M_PI))
				<<"\",\"length\":\""<<static_cast<int>(3*bus->length)
				<<"\",\"width\":\""<<static_cast<int>(2*bus->width)
				<<"\",\"passengers\":\""<<(bus?bus->getPassengerCount():0)
				<<"\",\"real_ArrivalTime\":\""<<(bus?real_ArrivalTime.get():0)
				<<"\",\"DwellTime_ijk\":\""<<(bus?DwellTime_ijk.get():0)
				<<"\",\"buslineID\":\""<<(bus?bus->getBusLineID():0) <<"\"})"<<std::endl);
	} else {
		LogOut(
				"(\"BusDriver\""
				<<","<<p.now.frame()
				<<","<<parent->getId()
				<<",{" <<"\"xPos\":\""<<static_cast<int>(bus->getX())
				<<"\",\"yPos\":\""<<static_cast<int>(bus->getY())
				<<"\",\"angle\":\""<<(360 - (baseAngle * 180 / M_PI))
				<<"\",\"length\":\""<<static_cast<int>(3*bus->length)
				<<"\",\"width\":\""<<static_cast<int>(2*bus->width)
				<<"\",\"passengers\":\""<<(bus?bus->getPassengerCountOld():0)
				<<"\",\"real_ArrivalTime\":\""<<(bus?real_ArrivalTime.get():0)
				<<"\",\"DwellTime_ijk\":\""<<(bus?DwellTime_ijk.get():0)
				<<"\",\"buslineID\":\""<<(bus?bus->getBusLineID():0) <<"\"})"<<std::endl);
	}

#endif
}

void sim_mob::BusDriver::frame_tick_output_mpi(timeslice now) {
	//Skip output?
	if (now.frame() < parent->getStartTime() || vehicle->isDone()) {
		return;
	}

	if (ConfigParams::GetInstance().OutputEnabled()) {
		double baseAngle =
				vehicle->isInIntersection() ?
						intModel->getCurrentAngle() : vehicle->getAngle();
//The BusDriver class will only maintain buses as the current vehicle.
		const Bus* bus = dynamic_cast<const Bus*>(vehicle);
		std::stringstream logout;
		if (!passengerCountOld_display_flag) {
			logout << "(\"Driver\"" << "," << now.frame() << ","
					<< parent->getId() << ",{" << "\"xPos\":\""
					<< static_cast<int>(bus->getX()) << "\",\"yPos\":\""
					<< static_cast<int>(bus->getY()) << "\",\"segment\":\""
					<< bus->getCurrSegment()->getId() << "\",\"angle\":\""
					<< (360 - (baseAngle * 180 / M_PI)) << "\",\"length\":\""
					<< static_cast<int>(bus->length) << "\",\"width\":\""
					<< static_cast<int>(bus->width) << "\",\"passengers\":\""
					<< (bus ? bus->getPassengerCount() : 0);
		} else {
			logout << "(\"Driver\"" << "," << now.frame() << ","
					<< parent->getId() << ",{" << "\"xPos\":\""
					<< static_cast<int>(bus->getX()) << "\",\"yPos\":\""
					<< static_cast<int>(bus->getY()) << "\",\"segment\":\""
					<< bus->getCurrSegment()->getId() << "\",\"angle\":\""
					<< (360 - (baseAngle * 180 / M_PI)) << "\",\"length\":\""
					<< static_cast<int>(bus->length) << "\",\"width\":\""
					<< static_cast<int>(bus->width) << "\",\"passengers\":\""
					<< (bus ? bus->getPassengerCountOld() : 0);
		}

		if (this->parent->isFake) {
			logout << "\",\"fake\":\"" << "true";
		} else {
			logout << "\",\"fake\":\"" << "false";
		}

		logout << "\"})" << std::endl;

		LogOut(logout.str());
	}
}

vector<BufferedBase*> sim_mob::BusDriver::getSubscriptionParams() {
	vector<BufferedBase*> res;
	res = Driver::getSubscriptionParams();

	// BusDriver's features
	res.push_back(&(lastVisited_BusStop));
	res.push_back(&(real_DepartureTime));
	res.push_back(&(real_ArrivalTime));
	res.push_back(&(DwellTime_ijk));
	res.push_back(&(busstop_sequence_no));
	res.push_back(curr_busStopRealTimes);

	for(int j = 0; j < busStopRealTimes_vec_bus.size(); j++) {
		res.push_back(busStopRealTimes_vec_bus[j]);
	}

	return res;
}

void sim_mob::BusDriver::AlightingPassengers(Bus* bus)//for alighting passengers
{
	if (bus->getPassengerCount() > 0) {
		int i = 0;
		vector<Person*>::iterator itr = bus->passengers_inside_bus.begin();
		while (itr != bus->passengers_inside_bus.end())
		{
			//Retrieve only Passenger agents inside the bus
			Person* p = bus->passengers_inside_bus[i];
			Passenger* passenger =p ? dynamic_cast<Passenger*>(p->getRole()) : nullptr;
			if (!passenger)
				continue;
			if (passenger->isBusBoarded() == true) //alighting is only for a passenger who has boarded the bus
			{
				if (passenger->PassengerAlightBus(bus, xpos_approachingbusstop,ypos_approachingbusstop, this) == true) //check if passenger wants to alight the bus
				{
					itr = (bus->passengers_inside_bus).erase(bus->passengers_inside_bus.begin() + i);
					no_passengers_alighting++;
				}
				else
				{
					itr++;
					i++;
				}
			}
		}
	}
}

void sim_mob::BusDriver::BoardingPassengers(Bus* bus) //boarding passengers
{
	vector<const Agent*> nearby_agents = AuraManager::instance().agentsInRect(
			Point2D((lastVisited_BusStop.get()->xPos - 3500),
					(lastVisited_BusStop.get()->yPos - 3500)),
			Point2D((lastVisited_BusStop.get()->xPos + 3500),
					(lastVisited_BusStop.get()->yPos + 3500))); //  nearbyAgents(Point2D(lastVisited_BusStop.get()->xPos, lastVisited_BusStop.get()->yPos), *params.currLane,3500,3500);

	for (vector<const Agent*>::iterator it = nearby_agents.begin(); it != nearby_agents.end(); it++)
	{
		//Retrieve only Passenger agents.
		const Person* person = dynamic_cast<const Person *>(*it);
		Person* p = const_cast<Person *>(person);
		Passenger* passenger =p ? dynamic_cast<Passenger*>(p->getRole()) : nullptr;
		if (!passenger)
			continue;
		if((abs((passenger->getXYPosition().getX()/1000)-(xpos_approachingbusstop/1000)) <=2) and (abs((passenger->getXYPosition().getY()/1000)-(ypos_approachingbusstop/1000))<=2) )
		{
			if (passenger->isAtBusStop() == true) //if passenger agent is waiting at the approaching bus stop
			{
				std::cout<<"x"<<passenger->getXYPosition().getX()<<std::endl;
				std::cout<<"y"<<passenger->getXYPosition().getY()<<std::endl;
				std::cout<<"seq"<<busstop_sequence_no.get()<<std::endl;
				 if(passenger->PassengerBoardBus(bus,this,p,busStops,busstop_sequence_no.get()+1)==true)//check if passenger wants to board the bus
					no_passengers_boarding++; //set the number of boarding passengers
			}

		}
	}
}
