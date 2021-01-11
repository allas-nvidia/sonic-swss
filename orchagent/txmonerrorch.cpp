#include "txmonerrorch.h"
#include "logger.h"
#include "portsorch.h"
#include "redisclient.h"
#include "sai_serialize.h"
#include "converter.h"

using namespace std;
using namespace swss;

#define TXMONORCH_SEL_TIMER                             "TXMONORCH_SEL_TIMER"
#define TXMONORCH_APP_DB_TX_MON_TABLE_THRESHOLD_STRING  "threshold"
#define TXMONORCH_APP_DB_TX_MON_TABLE_COUNTER_STRING    "counter"
#define TXMONORCH_APP_DB_TX_MON_TABLE_OBJ_ID_STRING     "objid"
#define TXMONORCH_SATE_DB_TX_MON_TABLE_STATE_STRING     "state"

#define COUNTER_NAME "SAI_PORT_STAT_IF_OUT_ERRORS"
extern PortsOrch*     gPortsOrch;

void TxMonOrch::UpdateStateDbMonTxErrorTabe(const string &portAlias, enum txMonState state)
{
    vector<FieldValueTuple> fvt;
    fvt.emplace_back(TXMONORCH_SATE_DB_TX_MON_TABLE_STATE_STRING, stateNames[state]);
    m_stateMonTxErrorTabe.set(portAlias, fvt);

}

void TxMonOrch:: pollErrorStatistics()
{
    vector<FieldValueTuple> fvt;
    txPortInfo portinfo;
    SWSS_LOG_ENTER();
    SWSS_LOG_INFO("TX_MON: pollErrorStatistics");
    for(auto &elem:mapInfo) {
        if (!pollErrorStatisticsForPort(elem.first, elem.second)) {
            continue;
        }
        portinfo = elem.second;
        fvt.emplace_back(TXMONORCH_APP_DB_TX_MON_TABLE_THRESHOLD_STRING, to_string(portinfo.threshold));
        fvt.emplace_back(TXMONORCH_APP_DB_TX_MON_TABLE_COUNTER_STRING, to_string(portinfo.counter));
        m_appMonTxErrorTable.set(elem.first, fvt);
    }

    m_appMonTxErrorTable.flush();
    m_stateMonTxErrorTabe.flush();
}

bool TxMonOrch::pollErrorStatisticsForPort(const string& portAlias,
                                           txPortInfo &portInfo)
{
    SWSS_LOG_ENTER();


    string cntStrVal;
    string oidStr = sai_serialize_object_id(portInfo.obj_id);

    SWSS_LOG_INFO("TX_MON: Read port %s DB counter value %lu\n", portAlias.c_str(),
                  portInfo.counter);

    if (!m_countersTable->hget(oidStr, COUNTER_NAME, cntStrVal))
    {
        SWSS_LOG_ERROR("Error reading counters table for port %s", portAlias.c_str());
        /*update state DB*/
        UpdateStateDbMonTxErrorTabe(portAlias, TXMON_PORT_STATE_INVALID);
        return false;
    }

    SWSS_LOG_INFO("TX_MON: Read port %s SAI counter value %s\n", portAlias.c_str(),
                  cntStrVal.c_str());

    /*check if counters pass threshold*/
    if (stoul(cntStrVal) - portInfo.counter > portInfo.threshold)
    {
        if(portInfo.state != stateNames[TXMON_PORT_STATE_NOT_OK])
        {
            portInfo.state  = stateNames[TXMON_PORT_STATE_NOT_OK];
            /*update state DB*/
            UpdateStateDbMonTxErrorTabe(portAlias, TXMON_PORT_STATE_NOT_OK);
        }
    } else {
        /*update state to OK*/
        if(portInfo.state != stateNames[TXMON_PORT_STATE_OK])
        {
            portInfo.state  = stateNames[TXMON_PORT_STATE_OK];
            /*update state DB*/
            UpdateStateDbMonTxErrorTabe(portAlias, TXMON_PORT_STATE_OK);
        }
    }

    /*update new counter value */
    portInfo.counter = stoul(cntStrVal);
    return true;
}

TxMonOrch::TxMonOrch(TableConnector appDbConnector,
              TableConnector confDbConnector,
              TableConnector stateDbConnector) :
              Orch(confDbConnector.first, confDbConnector.second),
              m_appMonTxErrorTable(appDbConnector.first, appDbConnector.second),
              m_stateMonTxErrorTabe(stateDbConnector.first, stateDbConnector.second),
              m_countersDb(new DBConnector("COUNTERS_DB", 0)),
              m_countersTable(new Table(m_countersDb.get(), COUNTERS_TABLE)),
                              mapInfo()
{
    auto interv = timespec { .tv_sec = 0, .tv_nsec = 0 };
    monTimer = new SelectableTimer(interv);
    auto executor = new ExecutableTimer(monTimer, this, TXMONORCH_SEL_TIMER);
    Orch::addExecutor(executor);
}

void TxMonOrch::ChangeTxErrMonInterval(vector<FieldValueTuple> fvt)
{
    try
    {
        for (auto idx : fvt)
        {
            auto m_txMonTimerInterval = stoi(fvValue(idx));

            auto interval = timespec { .tv_sec = m_txMonTimerInterval, .tv_nsec = 0 };
            monTimer->setInterval(interval);
            monTimer->reset();
            SWSS_LOG_NOTICE("Changing timer interval value to %s seconds", fvValue(idx).c_str());
        }

    }
    catch (...) {
        SWSS_LOG_WARN("Input timer interval value is invalid.");

    }
}

bool TxMonOrch::ChangeTxErrMonThreshold(string &portAlias, vector<FieldValueTuple> data)
{
    Port port;
    try
    {
        for (auto idx : data)
        {
            const auto &field = fvField(idx);
            const auto &value = fvValue(idx);

            if (field == "threshold") {
                if (value == "0") {
                    /*Delete from internal DB*/
                    mapInfo.erase(portAlias);
                    /*Delete from m_appMonTxErrorTable*/
                    m_appMonTxErrorTable.del(portAlias);
                    /*Delete from m_stateMonTxErrorTabe*/
                    m_stateMonTxErrorTabe.del(portAlias);;
                    SWSS_LOG_INFO("TXMON threshold cleared for port %s\n", portAlias.c_str());
                    return true;
                } else {
                    txPortInfo &portInfo = mapInfo[portAlias];
                    if (gPortsOrch->getPort(portAlias, port)) {
                        if (portInfo.obj_id == 0) {
                            portInfo.obj_id = port.m_port_id;
                            portInfo.state = stateNames[TXMON_PORT_STATE_UNKNOWN];
                        } else if (port.m_port_id != portInfo.obj_id) {
  //                          SWSS_LOG_ERROR("ChangeTxErrMonThreshold : Port %s has inconsistency : "
  //                                  "obj_ID in MAIN DB %d, in internal DB %d\n", portAlias.c_str(), <uint32_t>port.m_port_id, <uint32_t>portInfo.obj_id);
                            SWSS_LOG_ERROR("ChangeTxErrMonThreshold : Port %s has inconsistency : \n", portAlias.c_str());
                            return false;
                        }
                        portInfo.threshold = to_uint<uint32_t>(value);

                        /*UPDATE APP_DB*/
                        vector<FieldValueTuple> fvt;
                        fvt.emplace_back(TXMONORCH_APP_DB_TX_MON_TABLE_THRESHOLD_STRING, to_string(portInfo.threshold));
                        fvt.emplace_back(TXMONORCH_APP_DB_TX_MON_TABLE_COUNTER_STRING, to_string(portInfo.counter));
                        fvt.emplace_back(TXMONORCH_SATE_DB_TX_MON_TABLE_STATE_STRING, portInfo.state);
                        fvt.emplace_back(TXMONORCH_APP_DB_TX_MON_TABLE_OBJ_ID_STRING, to_string(portInfo.obj_id));


                        m_appMonTxErrorTable.set(portAlias, fvt);
                        m_appMonTxErrorTable.flush();
                    } else {
                        SWSS_LOG_INFO("Port wasn't found in main DB %s\n", portAlias.c_str());
                        return false;
                    }

                }
            }
        }
    }

    catch (...) {
        SWSS_LOG_WARN("Input timer interval value is invalid.");

    }
    return true;
}

TxMonOrch::~TxMonOrch(void)
{
    SWSS_LOG_ENTER();
}

void TxMonOrch::doTask(SelectableTimer &timer)
{
    SWSS_LOG_INFO("TxMonOrch doTask selectable timer\n");
    pollErrorStatistics();
}



void TxMonOrch::doTask(Consumer &consumer)
{
    SWSS_LOG_ENTER();

    bool status = true;

    if (!gPortsOrch->allPortsReady())
    {
        return;
    }

    auto it = consumer.m_toSync.begin();
    while (it != consumer.m_toSync.end())
    {
        KeyOpFieldsValuesTuple &t = it->second;

        string key = kfvKey(t);
        string op = kfvOp(t);

        vector<FieldValueTuple> fvt = kfvFieldsValues(t);

        SWSS_LOG_INFO("TX_MON: Configuration key %s op %s\n",
                      key.c_str(),
                      op.c_str());

        /*check if setting global period*/
        if (key == "timer_interval")
        {
            SWSS_LOG_INFO("TX_MON: TxMonHandleTimerIntervalUpdate Enter\n");
            ChangeTxErrMonInterval(fvt);
        } else {
            /*set threshold per port*/
            if (op == SET_COMMAND)
            {
                SWSS_LOG_INFO("TX_MON: TxMonHandleThresholdUpdate Enter");
                status = ChangeTxErrMonThreshold(key, fvt);
            }
        }

        if (!status) {
            SWSS_LOG_ERROR("Configuration failed for key %s.", key.c_str());
        }
        consumer.m_toSync.erase(it++);
    }
}


