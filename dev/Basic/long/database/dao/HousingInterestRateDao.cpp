/*
 * HousingInterestRateDao.cpp
 *
 *  Created on: 4 May, 2015
 *  Author: Chetan Rogbeer <chetan.rogbeer@smart.mit.edu>
 */

#include "HousingInterestRateDao.hpp"
#include "DatabaseHelper.hpp"

using namespace sim_mob::db;
using namespace sim_mob::long_term;

HousingInterestRateDao::HousingInterestRateDao(DB_Connection& connection): SqlAbstractDao<HousingInterestRate>( connection, "", "", "", "",	"SELECT * FROM " + connection.getSchema()+"housing_interest_rates", "" ){}

HousingInterestRateDao::~HousingInterestRateDao(){}

void HousingInterestRateDao::fromRow(Row& result, HousingInterestRate& outObj)
{
    outObj.id	= result.get<BigSerial>( "id", INVALID_ID);
    outObj.interest_rate	= result.get<double>("interest_rate", .0);
	outObj.year	        =result.get<int>( "year", 0);
	outObj.quarter	    =result.get<int>( "quarter", 0);
	outObj.yq	        =result.get<std::string>( "yq", ""); 
	outObj.infl_tminus1	=result.get<float>( "infl_tminus1", .0);
	outObj.infl_tplus1	=result.get<float>( "infl_tplus1", .0);
	outObj.gdp_growth	=result.get<float>( "gdp_growth", .0);
	outObj.rate_real	=result.get<float>( "rate_real", .0);
	outObj.source	    =result.get<std::string>( "source","");

}

void HousingInterestRateDao::toRow(HousingInterestRate& data, Parameters& outParams, bool update) {}
