
#include "vrepMainHeader.h"
#include "funcDebug.h"
#include "easyLock.h"
#include "v_rep_internal.h"
#include "luaScriptObject.h"
#include "luaScriptFunctions.h"
#include "threadPool.h"
#include "tt.h"
#include <boost/lexical_cast.hpp>
#include "vDateTime.h"
#include "app.h"
#include "apiErrors.h"
#include "pluginContainer.h"

int CLuaScriptObject::_nextIdForExternalScriptEditor=-1;
bool CLuaScriptObject::emergencyStopButtonPressed=false;
VMutex CLuaScriptObject::_globalMutex;
std::vector<CLuaScriptObject*> CLuaScriptObject::toBeCalledByThread;

CLuaScriptObject::CLuaScriptObject(int scriptTypeOrMinusOneForSerialization)
{
    scriptID=SIM_IDSTART_LUASCRIPT;
    _objectIDAttachedTo_child=-1; // used for child scripts
    _objectIDAttachedTo_callback=-1; // used for callback scripts
    _objectIDAttachedTo_customization=-1; // used for customization scripts

    _scriptText="";
    _scriptTextExec="";

    _errorReportMode=sim_api_error_output|sim_api_warning_output;
    _lastErrorString=SIM_API_CALL_NO_ERROR;
    _numberOfPasses=0;
    _threadedExecution=false;
    _scriptIsDisabled=false;
    _executeJustOnce=false;
    _disableCustomizationScriptWithError=true;
    _automaticCascadingCallsDisabled_OLD=false;
    _threadedExecutionUnderWay=false;
    _insideCustomLuaFunction=0;
    _flaggedForDestruction=false;
    _mainScriptIsDefaultMainScript=true;
    _executionOrder=sim_scriptexecorder_normal;
    _customizationScriptIsTemporarilyDisabled=false;
    _custScriptDisabledDSim_compatibilityMode=false;
    _customizationScriptCleanupBeforeSave=false;
    _addOn_executionState=sim_syscb_init;

    _previousEditionWindowPosAndSize[0]=50;
    _previousEditionWindowPosAndSize[1]=50;
    _previousEditionWindowPosAndSize[2]=1000;
    _previousEditionWindowPosAndSize[3]=800;

    _warningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014=false;
    _warning_simRMLPosition_oldCompatibility_30_8_2014=false;
    _warning_simRMLVelocity_oldCompatibility_30_8_2014=false;
    _warning_simGetMpConfigForTipPose_oldCompatibility_21_1_2016=false;
    _warning_simFindIkPath_oldCompatibility_2_2_2016=false;
    _warning_oldPathPlanningFunctionality_oldCompatibility_11_2_2016=false;
    _warning_oldMotionPlanningFunctionality_oldCompatibility_11_2_2016=false;
    _scriptExecStartTime=-1;

    _customObjectData=NULL;
    _customObjectData_tempData=NULL;

    scriptParameters=new CLuaScriptParameters();
    _outsideCommandQueue=new COutsideCommandQueueForScript();

    _scriptType=scriptTypeOrMinusOneForSerialization;
    _compatibilityModeOrFirstTimeCall_sysCallbacks=true;
    _containsJointCallbackFunction=false;
    _containsContactCallbackFunction=false;


    L=NULL;
    _loadBufferResult=-1;
    _inExecutionNow=false;

    if (_nextIdForExternalScriptEditor==-1)
    { // new since 10/9/2014, otherwise there can be conflicts between simultaneously opened V-REP instances
        _nextIdForExternalScriptEditor=(VDateTime::getOSTimeInMs()&0xffff)*1000;
    }

    _filenameForExternalScriptEditor="embScript_"+tt::FNb(_nextIdForExternalScriptEditor++)+".lua";

    if (_scriptType==sim_scripttype_sandboxscript)
    {
        scriptID=SIM_IDSTART_SANDBOXSCRIPT;
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        L=initializeNewLuaState(getScriptSuffixNumberString().c_str());
        std::string tmp("sim_current_script_id=");
        tmp+=boost::lexical_cast<std::string>(getScriptID());
        luaWrap_luaL_dostring(L,tmp.c_str());
    }
}

CLuaScriptObject::~CLuaScriptObject()
{
    FUNCTION_DEBUG;
    killLuaState(); // should already have been done outside of the destructor!
    clearAllUserData();
    delete scriptParameters;
    delete _outsideCommandQueue;
    delete _customObjectData;
    delete _customObjectData_tempData;
    if (!App::userSettings->useBuiltInScriptEditor())
    {
        if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback) )
        {       
            // destroy file
            std::string fname=App::directories->extScriptEditorTempFileDirectory+VREP_SLASH;
            fname.append(_filenameForExternalScriptEditor);
            if (VFile::doesFileExist(fname))
                VFile::eraseFile(fname);
        }
    }
}

std::string CLuaScriptObject::getSystemCallbackString(int calltype,bool callTips)
{
    if (calltype==sim_syscb_init)
    {
        std::string r("sysCall_init");
        if (callTips)
            r+="=()\nCalled when the script is initialized.";
        return(r);
    }
    if (calltype==sim_syscb_cleanup)
    {
        std::string r("sysCall_cleanup");
        if (callTips)
            r+="=()\nCalled when the script is destroyed.";
        return(r);
    }
    if (calltype==sim_syscb_nonsimulation)
    {
        std::string r("sysCall_nonSimulation");
        if (callTips)
            r+="=()\nCalled when simulation is not running.";
        return(r);
    }
    if (calltype==sim_syscb_beforesimulation)
    {
        std::string r("sysCall_beforeSimulation");
        if (callTips)
            r+="=()\nCalled just before simulation starts.";
        return(r);
    }
    if (calltype==sim_syscb_aftersimulation)
    {
        std::string r("sysCall_afterSimulation");
        if (callTips)
            r+="=()\nCalled just after simulation ended.";
        return(r);
    }
    if (calltype==sim_syscb_actuation)
    {
        std::string r("sysCall_actuation");
        if (callTips)
            r+="=()\nCalled in the actuation phase.";
        return(r);
    }
    if (calltype==sim_syscb_sensing)
    {
        std::string r("sysCall_sensing");
        if (callTips)
            r+="=()\nCalled in the sensing phase.";
        return(r);
    }
    if (calltype==sim_syscb_suspended)
    {
        std::string r("sysCall_suspended");
        if (callTips)
            r+="=()\nCalled when simulation is suspended.";
        return(r);
    }
    if (calltype==sim_syscb_suspend)
    {
        std::string r("sysCall_suspend");
        if (callTips)
            r+="=()\nCalled just before simulation gets suspended.";
        return(r);
    }
    if (calltype==sim_syscb_resume)
    {
        std::string r("sysCall_resume");
        if (callTips)
            r+="=()\nCalled just before simulation resumes.";
        return(r);
    }
    if (calltype==sim_syscb_beforeinstanceswitch)
    {
        std::string r("sysCall_beforeInstanceSwitch");
        if (callTips)
            r+="=()\nCalled just before an instance switch.";
        return(r);
    }
    if (calltype==sim_syscb_afterinstanceswitch)
    {
        std::string r("sysCall_afterInstanceSwitch");
        if (callTips)
            r+="=()\nCalled just after an instance switch.";
        return(r);
    }
    if (calltype==sim_syscb_beforecopy)
    {
        std::string r("sysCall_beforeCopy");
        if (callTips)
            r+="=()\nCalled just before objects are copied.";
        return(r);
    }
    if (calltype==sim_syscb_aftercopy)
    {
        std::string r("sysCall_afterCopy");
        if (callTips)
            r+="=()\nCalled just after objects were copied.";
        return(r);
    }
    if (calltype==sim_syscb_beforedelete)
    {
        std::string r("sysCall_beforeDelete");
        if (callTips)
            r+="=()\nCalled just before objects are deleted.";
        return(r);
    }
    if (calltype==sim_syscb_afterdelete)
    {
        std::string r("sysCall_afterDelete");
        if (callTips)
            r+="=()\nCalled just after objects were deleted.";
        return(r);
    }
    if (calltype==sim_syscb_aftercreate)
    {
        std::string r("sysCall_afterCreate");
        if (callTips)
            r+="=()\nCalled just after objects were created.";
        return(r);
    }
    if (calltype==sim_syscb_aos_run)
    {
        std::string r("sysCall_addOnScriptRun");
        if (callTips)
            r+="=()\nCalled when the add-on script is running.";
        return(r);
    }
    if (calltype==sim_syscb_aos_suspend)
    {
        std::string r("sysCall_addOnScriptSuspend");
        if (callTips)
            r+="=()\nCalled just before the add-on script execution gets suspended.";
        return(r);
    }
    if (calltype==sim_syscb_aos_resume)
    {
        std::string r("sysCall_addOnScriptResume");
        if (callTips)
            r+="=()\nCalled just before the add-on script execution resumes.";
        return(r);
    }
    if (calltype==sim_syscb_jointcallback)
    {
        std::string r("sysCall_jointCallback");
        if (callTips)
            r+="=()\nCalled after a dynamic simulation step.";
        return(r);
    }
    if (calltype==sim_syscb_contactcallback)
    {
        std::string r("sysCall_contactCallback");
        if (callTips)
            r+="=()\nCalled by the physics engine when two respondable shapes are contacting.";
        return(r);
    }
    if ( (calltype>=sim_syscb_customcallback1)&&(calltype<=sim_syscb_customcallback4) )
    {
        std::string r("sysCall_customCallback");
        r+=boost::lexical_cast<std::string>(calltype-sim_syscb_customcallback1+1);
        if (callTips)
            r+="=()\nCan be called by a customized main script.";
        return(r);
    }
    if (calltype==sim_syscb_threadmain)
    {
        std::string r("sysCall_threadmain");
        if (callTips)
            r+="=()\nMain function of a threaded child script.";
        return(r);
    }
    if (calltype==sim_syscb_br)
    {
        std::string r("sysCall_br");
        if (callTips)
            r+="=(...)\nCalled for BlueReality functionality.";
        return(r);
    }
    return("");
}

bool CLuaScriptObject::canCallSystemCallback(int scriptType,bool threaded,int callType)
{
    if (scriptType==-1)
        return(true);
    if (scriptType==sim_scripttype_addonscript)
    {
        if (callType==sim_syscb_init)
            return(true);
        if (callType==sim_syscb_cleanup)
            return(true);
        if (callType==sim_syscb_aos_run)
            return(true);
        if (callType==sim_syscb_aos_suspend)
            return(true);
        if (callType==sim_syscb_aos_resume)
            return(true);
        if (callType==sim_syscb_beforesimulation)
            return(true);
        if (callType==sim_syscb_aftersimulation)
            return(true);
        if (callType==sim_syscb_suspend)
            return(true);
        if (callType==sim_syscb_resume)
            return(true);
        if (callType==sim_syscb_beforecopy)
            return(true);
        if (callType==sim_syscb_aftercopy)
            return(true);
        if (callType==sim_syscb_beforedelete)
            return(true);
        if (callType==sim_syscb_afterdelete)
            return(true);
        if (callType==sim_syscb_aftercreate)
            return(true);
        if (callType==sim_syscb_beforeinstanceswitch)
            return(true);
        if (callType==sim_syscb_afterinstanceswitch)
            return(true);
        if (callType==sim_syscb_br)
            return(true);
    }
    if (scriptType==sim_scripttype_customizationscript)
    {
        if (callType==sim_syscb_init)
            return(true);
        if (callType==sim_syscb_cleanup)
            return(true);
        if (callType==sim_syscb_nonsimulation)
            return(true);
        if (callType==sim_syscb_beforesimulation)
            return(true);
        if (callType==sim_syscb_aftersimulation)
            return(true);
        if (callType==sim_syscb_actuation)
            return(true);
        if (callType==sim_syscb_sensing)
            return(true);
        if (callType==sim_syscb_suspend)
            return(true);
        if (callType==sim_syscb_suspended)
            return(true);
        if (callType==sim_syscb_resume)
            return(true);
        if (callType==sim_syscb_beforecopy)
            return(true);
        if (callType==sim_syscb_aftercopy)
            return(true);
        if (callType==sim_syscb_beforedelete)
            return(true);
        if (callType==sim_syscb_afterdelete)
            return(true);
        if (callType==sim_syscb_aftercreate)
            return(true);
        if (callType==sim_syscb_beforeinstanceswitch)
            return(true);
        if (callType==sim_syscb_afterinstanceswitch)
            return(true);
        if (callType==sim_syscb_br)
            return(true);
        if (callType==sim_syscb_jointcallback)
            return(true);
        if (callType==sim_syscb_contactcallback)
            return(true);
        if ( (callType>=sim_syscb_customcallback1)&&(callType<=sim_syscb_customcallback4) )
            return(true);
    }
    if (scriptType==sim_scripttype_mainscript)
    {
        if (callType==sim_syscb_init)
            return(true);
        if (callType==sim_syscb_cleanup)
            return(true);
        if (callType==sim_syscb_actuation)
            return(true);
        if (callType==sim_syscb_sensing)
            return(true);
        if (callType==sim_syscb_suspend)
            return(true);
        if (callType==sim_syscb_resume)
            return(true);
        if (callType==sim_syscb_beforecopy)
            return(true);
        if (callType==sim_syscb_aftercopy)
            return(true);
        if (callType==sim_syscb_beforedelete)
            return(true);
        if (callType==sim_syscb_afterdelete)
            return(true);
        if (callType==sim_syscb_aftercreate)
            return(true);
    }
    if (scriptType==sim_scripttype_childscript)
    {
        if (callType==sim_syscb_cleanup)
            return(true);
        if (threaded)
        {
            if (callType==sim_syscb_threadmain)
                return(true);
        }
        else
        {
            if (callType==sim_syscb_init)
                return(true);
            if (callType==sim_syscb_actuation)
                return(true);
            if (callType==sim_syscb_sensing)
                return(true);
            if (callType==sim_syscb_suspend)
                return(true);
            if (callType==sim_syscb_resume)
                return(true);
            if (callType==sim_syscb_beforecopy)
                return(true);
            if (callType==sim_syscb_aftercopy)
                return(true);
            if (callType==sim_syscb_beforedelete)
                return(true);
            if (callType==sim_syscb_afterdelete)
                return(true);
            if (callType==sim_syscb_aftercreate)
                return(true);
            if (callType==sim_syscb_jointcallback)
                return(true);
            if (callType==sim_syscb_contactcallback)
                return(true);
            if ( (callType>=sim_syscb_customcallback1)&&(callType<=sim_syscb_customcallback4) )
                return(true);
        }
    }
    return(false);
}

std::vector<std::string> CLuaScriptObject::getAllSystemCallbackStrings(int scriptType,bool threaded,bool callTips)
{
    std::vector<std::string> retVal;
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_init))
        retVal.push_back(getSystemCallbackString(sim_syscb_init,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_cleanup))
        retVal.push_back(getSystemCallbackString(sim_syscb_cleanup,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_nonsimulation))
        retVal.push_back(getSystemCallbackString(sim_syscb_nonsimulation,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_beforesimulation))
        retVal.push_back(getSystemCallbackString(sim_syscb_beforesimulation,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aftersimulation))
        retVal.push_back(getSystemCallbackString(sim_syscb_aftersimulation,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_actuation))
        retVal.push_back(getSystemCallbackString(sim_syscb_actuation,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_sensing))
        retVal.push_back(getSystemCallbackString(sim_syscb_sensing,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_suspended))
        retVal.push_back(getSystemCallbackString(sim_syscb_suspended,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_suspend))
        retVal.push_back(getSystemCallbackString(sim_syscb_suspend,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_resume))
        retVal.push_back(getSystemCallbackString(sim_syscb_resume,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_beforeinstanceswitch))
        retVal.push_back(getSystemCallbackString(sim_syscb_beforeinstanceswitch,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_afterinstanceswitch))
        retVal.push_back(getSystemCallbackString(sim_syscb_afterinstanceswitch,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_beforecopy))
        retVal.push_back(getSystemCallbackString(sim_syscb_beforecopy,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aftercopy))
        retVal.push_back(getSystemCallbackString(sim_syscb_aftercopy,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_beforedelete))
        retVal.push_back(getSystemCallbackString(sim_syscb_beforedelete,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_afterdelete))
        retVal.push_back(getSystemCallbackString(sim_syscb_afterdelete,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aftercreate))
        retVal.push_back(getSystemCallbackString(sim_syscb_aftercreate,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aos_run))
        retVal.push_back(getSystemCallbackString(sim_syscb_aos_run,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aos_suspend))
        retVal.push_back(getSystemCallbackString(sim_syscb_aos_suspend,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_aos_resume))
        retVal.push_back(getSystemCallbackString(sim_syscb_aos_resume,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_jointcallback))
        retVal.push_back(getSystemCallbackString(sim_syscb_jointcallback,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_contactcallback))
        retVal.push_back(getSystemCallbackString(sim_syscb_contactcallback,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_customcallback1))
        retVal.push_back(getSystemCallbackString(sim_syscb_customcallback1,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_customcallback2))
        retVal.push_back(getSystemCallbackString(sim_syscb_customcallback2,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_customcallback3))
        retVal.push_back(getSystemCallbackString(sim_syscb_customcallback3,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_customcallback4))
        retVal.push_back(getSystemCallbackString(sim_syscb_customcallback4,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_threadmain))
        retVal.push_back(getSystemCallbackString(sim_syscb_threadmain,callTips));
    if (canCallSystemCallback(scriptType,threaded,sim_syscb_br))
        retVal.push_back(getSystemCallbackString(sim_syscb_br,callTips));
    return(retVal);
}

void CLuaScriptObject::setInsideCustomLuaFunction(bool inside)
{
    if (inside)
        _insideCustomLuaFunction++;
    else
        _insideCustomLuaFunction--;
}

bool CLuaScriptObject::getInsideCustomLuaFunction() const
{
    return(_insideCustomLuaFunction>0);
}

bool CLuaScriptObject::getContainsJointCallbackFunction() const
{
    return(_containsJointCallbackFunction);
}

bool CLuaScriptObject::getContainsContactCallbackFunction() const
{
    return(_containsContactCallbackFunction);
}

int CLuaScriptObject::getErrorReportMode() const
{
    return(_errorReportMode);
}

void CLuaScriptObject::setErrorReportMode(int e)
{
    _errorReportMode=e;
}

std::string CLuaScriptObject::getLastErrorString() const
{
    return(_lastErrorString);
}

void CLuaScriptObject::setLastErrorString(const char* txt)
{
    _lastErrorString=txt;
}

int CLuaScriptObject::getScriptExecutionTimeInMs() const
{
    if (_scriptExecStartTime<0) // happens sometimes when calling luaWrap_luaL_doString
        return(0);
    return(VDateTime::getTimeDiffInMs(_scriptExecStartTime));
}

void CLuaScriptObject::setObjectCustomData(int header,const char* data,int dataLength)
{
    if (_customObjectData==NULL)
        _customObjectData=new CCustomData();
    _customObjectData->setData(header,data,dataLength);
}
int CLuaScriptObject::getObjectCustomDataLength(int header) const
{
    if (_customObjectData==NULL)
        return(0);
    return(_customObjectData->getDataLength(header));
}
void CLuaScriptObject::getObjectCustomData(int header,char* data) const
{
    if (_customObjectData==NULL)
        return;
    _customObjectData->getData(header,data);
}

bool CLuaScriptObject::getObjectCustomDataHeader(int index,int& header) const
{
    if (_customObjectData==NULL)
        return(false);
    return(_customObjectData->getHeader(index,header));
}



void CLuaScriptObject::setObjectCustomData_tempData(int header,const char* data,int dataLength)
{
    if (_customObjectData_tempData==NULL)
        _customObjectData_tempData=new CCustomData();
    _customObjectData_tempData->setData(header,data,dataLength);
}
int CLuaScriptObject::getObjectCustomDataLength_tempData(int header) const
{
    if (_customObjectData_tempData==NULL)
        return(0);
    return(_customObjectData_tempData->getDataLength(header));
}
void CLuaScriptObject::getObjectCustomData_tempData(int header,char* data) const
{
    if (_customObjectData_tempData==NULL)
        return;
    _customObjectData_tempData->getData(header,data);
}

bool CLuaScriptObject::getObjectCustomDataHeader_tempData(int index,int& header) const
{
    if (_customObjectData_tempData==NULL)
        return(false);
    return(_customObjectData_tempData->getHeader(index,header));
}



void CLuaScriptObject::getPreviousEditionWindowPosAndSize(int posAndSize[4]) const
{
    for (int i=0;i<4;i++)
        posAndSize[i]=_previousEditionWindowPosAndSize[i];
}

void CLuaScriptObject::setPreviousEditionWindowPosAndSize(const int posAndSize[4])
{
    for (int i=0;i<4;i++)
        _previousEditionWindowPosAndSize[i]=posAndSize[i];
}

std::string CLuaScriptObject::getAddOnName() const
{
    return(_addOnName);
}

int CLuaScriptObject::getAddOnExecutionState() const
{
    return(_addOn_executionState);
}

std::string CLuaScriptObject::getFilenameForExternalScriptEditor() const
{
    std::string fname=App::directories->extScriptEditorTempFileDirectory+VREP_SLASH;
    fname.append(_filenameForExternalScriptEditor);
    return(fname);
}

void CLuaScriptObject::fromFileToBuffer()
{
    if (!App::userSettings->useBuiltInScriptEditor())
    { // read file
        _scriptFoldingInfo.clear();
        if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback) )
        {
            std::string fname=App::directories->extScriptEditorTempFileDirectory+VREP_SLASH;
            fname.append(_filenameForExternalScriptEditor);

            VFile myFile(fname.c_str(),VFile::READ|VFile::SHARE_DENY_NONE,true);
            if (myFile.getFile()!=NULL)
            {
                VArchive arW(&myFile,VArchive::LOAD);
                unsigned int archiveLength=(unsigned int)myFile.getLength();

                _scriptText.resize(archiveLength,' ');
                for (unsigned int i=0;i<archiveLength;i++)
                    arW >> _scriptText[i];

                arW.close();
                myFile.close();
            }
        }
    }
}

void CLuaScriptObject::fromBufferToFile()
{
    if (!App::userSettings->useBuiltInScriptEditor())
    { // write file
        _scriptFoldingInfo.clear();
        if ((App::ct->environment==NULL)||(!App::ct->environment->getSceneLocked()))
        {
            if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback) )
            {
                std::string fname=App::directories->extScriptEditorTempFileDirectory+VREP_SLASH;
                fname.append(_filenameForExternalScriptEditor);

                VFile myFile(fname.c_str(),VFile::CREATE_WRITE|VFile::SHARE_EXCLUSIVE,true);
                if (myFile.getFile()!=NULL)
                {
                    VArchive arW(&myFile,VArchive::STORE);
                    if (_scriptText.size()>0)
                    {
                        for (size_t i=0;i<_scriptText.size();i++)
                            arW << _scriptText[i];
                    }
                    arW.close();
                    myFile.close();
                }
            }
        }
    }
}

void CLuaScriptObject::setExecuteJustOnce(bool justOnce)
{
    _executeJustOnce=justOnce;
}

bool CLuaScriptObject::getExecuteJustOnce() const
{
    return(_executeJustOnce);
}

void CLuaScriptObject::initializeInitialValues(bool simulationIsRunning)
{ // is called at simulation start, but also after object(s) have been copied into a scene!
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback) )//||(_scriptType==sim_scripttype_generalcallback) )
    {
        _initialValuesInitialized=simulationIsRunning;
        if (simulationIsRunning)
        {

        }
        if (scriptParameters!=NULL)
            scriptParameters->initializeInitialValues(simulationIsRunning);
        if (_outsideCommandQueue!=NULL)
            _outsideCommandQueue->initializeInitialValues(simulationIsRunning);
    }
}

void CLuaScriptObject::simulationAboutToStart()
{
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback) )//||(_scriptType==sim_scripttype_generalcallback) )
    {
        killLuaState(); // should already be reset! (should have been done in simulationEnded routine)
        _numberOfPasses=0;
        _automaticCascadingCallsDisabled_OLD=false;
        initializeInitialValues(true);
        _warningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014=false;
        _warning_simRMLPosition_oldCompatibility_30_8_2014=false;
        _warning_simRMLVelocity_oldCompatibility_30_8_2014=false;
        _warning_simGetMpConfigForTipPose_oldCompatibility_21_1_2016=false;
        _warning_simFindIkPath_oldCompatibility_2_2_2016=false;
        _warning_oldPathPlanningFunctionality_oldCompatibility_11_2_2016=false;
        _warning_oldMotionPlanningFunctionality_oldCompatibility_11_2_2016=false;
    }
}

void CLuaScriptObject::simulationEnded()
{ // Remember, this is not guaranteed to be run! (the object can be copied during simulation, and pasted after it ended). For thoses situations there is the initializeInitialValues routine!
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback) )//||(_scriptType==sim_scripttype_generalcallback) )
    {
        if (scriptParameters!=NULL)
            scriptParameters->simulationEnded();
        if (_outsideCommandQueue!=NULL)
            _outsideCommandQueue->simulationEnded();
        _scriptTextExec.clear();
        clearAllUserData();
        if (_initialValuesInitialized&&App::ct->simulation->getResetSceneAtSimulationEnd())
        {

        }
        _initialValuesInitialized=false;
    }
}

void CLuaScriptObject::simulationAboutToEnd()
{ // Added this on 7/8/2014.
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback) ) // not for customization or general callback script!    ||(_scriptType==sim_scripttype_generalcallback) )// not for customization scripts!!  ||(_scriptType==sim_scripttype_customizationscript))
        killLuaState(); // this has to happen while simulation is still running!!
}

std::string CLuaScriptObject::errorWithCustomizationScript()
{
    if (_scriptType==sim_scripttype_customizationscript)
    {
        _customizationScriptIsTemporarilyDisabled=_disableCustomizationScriptWithError;
        if (_disableCustomizationScriptWithError)
            return("\n--> Customization script temporarily disabled.");
    }
    return("");
}

void CLuaScriptObject::setCustomizationScriptIsTemporarilyDisabled(bool disabled)
{
    _customizationScriptIsTemporarilyDisabled=disabled;
}

void CLuaScriptObject::setCustScriptDisabledDSim_compatibilityMode(bool disabled)
{
    _custScriptDisabledDSim_compatibilityMode=disabled;
}

bool CLuaScriptObject::getCustScriptDisabledDSim_compatibilityMode() const
{
    return(_custScriptDisabledDSim_compatibilityMode);
}

bool CLuaScriptObject::getCustomizationScriptIsTemporarilyDisabled() const
{
    return(_customizationScriptIsTemporarilyDisabled);
}

void CLuaScriptObject::setCustomizationScriptCleanupBeforeSave(bool doIt)
{
    _customizationScriptCleanupBeforeSave=doIt;
}

bool CLuaScriptObject::getCustomizationScriptCleanupBeforeSave() const
{
    return(_customizationScriptCleanupBeforeSave);
}

bool CLuaScriptObject::hasCustomizationScripAnyChanceToGetExecuted(bool whenSimulationRuns,bool forCleanUpSection) const
{
    // Following 2 since 23/1/2017
    if (forCleanUpSection)
        return(true);

    if (!App::userSettings->runCustomizationScripts)
        return(false);
    if (_scriptIsDisabled|_customizationScriptIsTemporarilyDisabled)
        return(false);
    if (whenSimulationRuns&&_custScriptDisabledDSim_compatibilityMode&&_compatibilityModeOrFirstTimeCall_sysCallbacks)
        return(false);
    C3DObject* obj=App::ct->objCont->getObject(getObjectIDThatScriptIsAttachedTo_customization());
    if (obj==NULL) // should never happen!
    { // can happen when the object attached to the script is already destroyed.
        // In that case we still want to run the customization script, at least the clean-up section of it!
 //       if (forCleanUpSection)
 //           return(true);
        return(false);
    }
    if (obj->getCumulativeModelProperty()&sim_modelproperty_scripts_inactive)
        return(false);
    return(true);
}

void CLuaScriptObject::setExecutionOrder(int order)
{
    _executionOrder=tt::getLimitedInt(0,2,order);
}

int CLuaScriptObject::getExecutionOrder() const
{
    return(_executionOrder);
}

void CLuaScriptObject::setCustomizedMainScript(bool customized)
{
    if (_scriptType==sim_scripttype_mainscript)
    {
        _mainScriptIsDefaultMainScript=!customized;
        App::setFullDialogRefreshFlag();
    }
}

bool CLuaScriptObject::isDefaultMainScript() const
{
    return(_mainScriptIsDefaultMainScript);
}

void CLuaScriptObject::setDisableCustomizationScriptWithError(bool d)
{
    _disableCustomizationScriptWithError=d;
}

bool CLuaScriptObject::getDisableCustomizationScriptWithError() const
{
    return(_disableCustomizationScriptWithError);
}

void CLuaScriptObject::setScriptIsDisabled(bool isDisabled)
{
    if (isDisabled)
    {
//        if (_scriptType!=sim_scripttype_mainscript)
            _scriptIsDisabled=true;
    }
    else
        _scriptIsDisabled=false;
}

bool CLuaScriptObject::getScriptIsDisabled() const
{
    return(_scriptIsDisabled);
}

int CLuaScriptObject::getScriptType() const
{
    return(_scriptType);
}

void CLuaScriptObject::flagForDestruction()
{
    _flaggedForDestruction=true;
}

bool CLuaScriptObject::getFlaggedForDestruction() const
{
    return(_flaggedForDestruction);
}

void CLuaScriptObject::setAddOnScriptAutoRun()
{
    _addOn_executionState=-1;
}

void CLuaScriptObject::setScriptText(const char* scriptTxt,const std::vector<int>* scriptFoldingInfo)
{
    EASYLOCK(_localMutex);

    _scriptText="";
    if (scriptTxt!=NULL)
        _scriptText=scriptTxt;

    if (scriptFoldingInfo!=NULL)
        _scriptFoldingInfo.assign(scriptFoldingInfo[0].begin(),scriptFoldingInfo[0].end());
    else
        _scriptFoldingInfo.clear();
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback) )
        fromBufferToFile();
}

const char* CLuaScriptObject::getScriptText(std::vector<int>* scriptFoldingInfo)
{
    if (scriptFoldingInfo!=NULL)
        scriptFoldingInfo[0].assign(_scriptFoldingInfo.begin(),_scriptFoldingInfo.end());
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback) )
        fromFileToBuffer();
    return(_scriptText.c_str());
}

int CLuaScriptObject::getScriptID() const
{
    return(scriptID);
}

void CLuaScriptObject::setScriptID(int newID)
{ // careful with that function!
    scriptID=newID;
}

bool CLuaScriptObject::isSceneScript() const
{
    return ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback)||(_scriptType==sim_scripttype_generalcallback)||(_scriptType==sim_scripttype_customizationscript) );
}

std::string CLuaScriptObject::getIncludeScriptFilePathAndName() const
{ // Will return a non-empty string only with script contents similar to following example:

    //sim.include('/BlueWorkforce/modelScripts/ragnar_child.lua')

    std::string stt(_scriptText);

    size_t le=stt.find(")");
    if ( (stt.compare(0,11,"sim.include")==0)&&(le!=std::string::npos) )
    {
        std::string filename(_scriptText.begin()+13,_scriptText.begin()+le-1);
        filename=App::directories->executableDirectory+filename;
        return(filename);
    }


    // Following is for backward compatibility:

    //require('utils')
    //include('/BlueWorkforce/modelScripts/ragnar_child.lua')
    //-- Do not add any code in here and leave above 2 lines in position

    size_t line2Start=stt.find('\n')+1;
    if ((line2Start!=std::string::npos))
    {
        std::string line1(_scriptText.begin(),_scriptText.begin()+line2Start);
        size_t line2End=stt.find(')',line2Start);
        if ((line2End!=std::string::npos)&&(line1.compare(0,7,"require")==0)&&(line2End-line2Start>8))
        {
            std::string line2(_scriptText.begin()+line2Start,_scriptText.begin()+line2End);
            size_t fileStart=stt.find('(',line2Start);
            if ((line2.compare(0,7,"include")==0)&&(fileStart!=std::string::npos)&&(fileStart<line2End))
            {
                std::string filename(_scriptText.begin()+fileStart+2,_scriptText.begin()+line2End-1);
                filename=App::directories->executableDirectory+filename;
                return(filename);
            }
        }
    }
    return("");
}

std::string CLuaScriptObject::getDescriptiveName() const
{ // Cannot put following strings to resources since they are also used in openGL!!!
    std::string pref;
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript) )
    {
        if (!App::userSettings->useBuiltInScriptEditor())
        {
            pref="[";
            pref+=_filenameForExternalScriptEditor;
            pref+="] ";
        }
    }

    if (_scriptType==sim_scripttype_mainscript)
    {
        if (_mainScriptIsDefaultMainScript)
            return(strTranslate(pref+"Main script (default)"));
        return(strTranslate(pref+"Main script (customized)"));
    }
    if (_scriptType==sim_scripttype_childscript)
    {
        if (_flaggedForDestruction)
        {
            if (_threadedExecution)
                return(strTranslate(pref+"Threaded child script (destroyed)"));
            return(strTranslate(pref+"Non-threaded Child script (destroyed)"));
        }
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_child);
        if (it==NULL)
        {
            if (_threadedExecution)
                return(strTranslate(pref+"Threaded child script (unassociated)"));
            return(strTranslate(pref+"Non-threaded child script (unassociated)"));
        }
        std::string retVal;
        if (_threadedExecution)
            retVal=strTranslate(pref+"Threaded child script (");
        else
            retVal=strTranslate(pref+"Non-threaded child script (");
        retVal+=it->getName();
        retVal+=")";
        return(retVal);
    }

    if (_scriptType==sim_scripttype_addonscript)
    {
        std::string retVal;
        retVal=strTranslate(pref+"Add-on script '");
        retVal+=_addOnName;
        retVal+="'";
        return(retVal);
    }
    if (_scriptType==sim_scripttype_addonfunction)
    {
        std::string retVal;
        retVal=strTranslate(pref+"Add-on function '");
        retVal+=_addOnName;
        retVal+="'";
        return(retVal);
    }
    if (_scriptType==sim_scripttype_jointctrlcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"Joint ctrl callback script ");
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_callback);
        if (it==NULL)
            return(strTranslate(pref+"Joint ctrl callback script (unassociated)"));

        retVal+="(";
        retVal+=it->getName();
        retVal+=")";
        return(retVal);
    }
    if (_scriptType==sim_scripttype_customizationscript)
    {
        std::string retVal;
        retVal=strTranslate(pref+"Customization script ");
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_customization);
        if (it==NULL)
            return(strTranslate(pref+"Customization script (unassociated)"));

        retVal+="(";
        retVal+=it->getName();
        retVal+=")";
        return(retVal);
    }
    if (_scriptType==sim_scripttype_contactcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"Contact callback script");
        return(retVal);
    }
    if (_scriptType==sim_scripttype_generalcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"General callback script");
        return(retVal);
    }
    if (_scriptType==sim_scripttype_sandboxscript)
        return(strTranslate(pref+"Sandbox script"));
    return("ERROR");
}

std::string CLuaScriptObject::getShortDescriptiveName() const
{ // since 2/10/2012 (because if the name is too long, it is truncated when an error message mentions it)
    std::string pref;
    if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript) )
    {
        if (!App::userSettings->useBuiltInScriptEditor())
        {
            pref="[";
            pref+=_filenameForExternalScriptEditor;
            pref+="] ";
        }
    }

    if (_scriptType==sim_scripttype_mainscript)
        return(strTranslate(pref+"MAIN SCRIPT"));
    if (_scriptType==sim_scripttype_childscript)
    {
        if (_flaggedForDestruction)
            return(strTranslate(pref+"CHILD SCRIPT (DESTROYED)"));
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_child);
        if (it==NULL)
            return(strTranslate(pref+"UNASSOCIATED CHILD SCRIPT"));

        std::string retVal;
        retVal=strTranslate(pref+"SCRIPT ");
        retVal+=it->getName();
        return(retVal);
    }
    if (_scriptType==sim_scripttype_addonscript)
    {
        std::string retVal;
        retVal=strTranslate(pref+"ADDON SCRIPT ");
        retVal+=_addOnName;
        return(retVal);
    }
    if (_scriptType==sim_scripttype_addonfunction)
    {
        std::string retVal;
        retVal=strTranslate(pref+"ADDON FUNCTION ");
        retVal+=_addOnName;
        return(retVal);
    }
    if (_scriptType==sim_scripttype_jointctrlcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"JOINT CTRL CALLBACK ");
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_callback);
        if (it==NULL)
            return(strTranslate(pref+"JOINT CTRL CALLBACK (UNASSOCIATED)"));
        retVal+=it->getName();
        return(retVal);
    }
    if (_scriptType==sim_scripttype_customizationscript)
    {
        std::string retVal;
        retVal=strTranslate(pref+"CUSTOMIZATION SCRIPT ");
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_customization);
        if (it==NULL)
            return(strTranslate(pref+"CUSTOMIZATION SCRIPT  (UNASSOCIATED)"));
        retVal+=it->getName();
        return(retVal);
    }
    if (_scriptType==sim_scripttype_contactcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"CONTACT CALLBACK");
        return(retVal);
    }
    if (_scriptType==sim_scripttype_generalcallback)
    {
        std::string retVal;
        retVal=strTranslate(pref+"GENERAL CALLBACK");
        return(retVal);
    }
    if (_scriptType==sim_scripttype_sandboxscript)
        return(strTranslate(pref+"SANDBOX SCRIPT"));
    return("ERROR");
}

void CLuaScriptObject::setAddOnName(const char* name)
{
    _addOnName=name;
}

std::string CLuaScriptObject::getScriptSuffixNumberString() const
{
    C3DObject* it=NULL;
    if (_scriptType==sim_scripttype_childscript)
        it=App::ct->objCont->getObject(_objectIDAttachedTo_child);
    if (_scriptType==sim_scripttype_jointctrlcallback)
        it=App::ct->objCont->getObject(_objectIDAttachedTo_callback);
    if (_scriptType==sim_scripttype_customizationscript)
        it=App::ct->objCont->getObject(_objectIDAttachedTo_customization);
    if (it==NULL)
        return("");
    int suffNb=tt::getNameSuffixNumber(it->getName().c_str(),true);
    std::string suffix("");
    if (suffNb!=-1)
        suffix=boost::lexical_cast<std::string>(suffNb);
    return(suffix);
}

std::string CLuaScriptObject::getScriptPseudoName() const
{
    if (_scriptType==sim_scripttype_childscript)
    {
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_child);
        if (it!=NULL)
            return(it->getName());
    }
    if ( (_scriptType==sim_scripttype_addonscript)||(_scriptType==sim_scripttype_addonfunction) )
        return(_addOnName);
    if (_scriptType==sim_scripttype_jointctrlcallback)
    {
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_callback);
        if (it!=NULL)
            return(it->getName());
    }
    if (_scriptType==sim_scripttype_customizationscript)
    {
        C3DObject* it=App::ct->objCont->getObject(_objectIDAttachedTo_customization);
        if (it!=NULL)
            return(it->getName());
    }
    return("");
}

void CLuaScriptObject::setThreadedExecution(bool threadedExec)
{
    if (threadedExec)
    {
        if (_scriptType==sim_scripttype_childscript)
            _threadedExecution=true;
    }
    else
        _threadedExecution=false;
}

bool CLuaScriptObject::getThreadedExecution() const
{
    return(_threadedExecution);
}

bool CLuaScriptObject::getThreadedExecutionIsUnderWay() const
{
    return(_threadedExecutionUnderWay);
}

void CLuaScriptObject::perform3DObjectLoadingMapping(std::vector<int>* map)
{
    if (App::ct->objCont!=NULL)
    {
        _objectIDAttachedTo_child=App::ct->objCont->getLoadingMapping(map,_objectIDAttachedTo_child);
        _objectIDAttachedTo_callback=App::ct->objCont->getLoadingMapping(map,_objectIDAttachedTo_callback);
        _objectIDAttachedTo_customization=App::ct->objCont->getLoadingMapping(map,_objectIDAttachedTo_customization);
    }
}

bool CLuaScriptObject::announce3DObjectWillBeErased(int objectID,bool copyBuffer)
{ // script will be erased if attached to objectID (if threaded simulation is not running!)
    if (_objectIDAttachedTo_child==objectID)
    {
        if (copyBuffer)
            return(true);
        if (!App::ct->simulation->isSimulationStopped()) // Removed the if(_threadedExecution()) thing on 2008/12/08
        { // threaded scripts cannot be directly erased, since the Lua state needs to be cleared in the thread that created it
#ifdef SIM_WITH_GUI
            if (App::mainWindow!=NULL)
                App::mainWindow->scintillaEditorContainer->closeEditor(scriptID);
#endif
            _objectIDAttachedTo_child=-1; // This is for a potential threaded simulation running
            _flaggedForDestruction=true;
            return(!_inExecutionNow); // from false to !_inExecutionNow on 8/9/2016
        }
        else
            return(true);
    }
    if (_objectIDAttachedTo_callback==objectID)
    {
        if (copyBuffer)
            return(true);
        _flaggedForDestruction=true;
#ifdef SIM_WITH_GUI
        if (App::mainWindow!=NULL)
            App::mainWindow->scintillaEditorContainer->closeEditor(scriptID);
#endif
        return(!_inExecutionNow); // from false to !_inExecutionNow on 26/8/2016 (i.e. no delayed destruction anymore)
    }
    if (_objectIDAttachedTo_customization==objectID)
    {
        if (copyBuffer)
            return(true);
        _flaggedForDestruction=true;
#ifdef SIM_WITH_GUI
        if (App::mainWindow!=NULL)
            App::mainWindow->scintillaEditorContainer->closeEditor(scriptID);
#endif
        return(!_inExecutionNow); // from false to !_inExecutionNow on 26/8/2016 (i.e. no delayed destruction anymore. Important since the clean-up section of custom. scripts can contain code that refers to the attached object, etc.)
    }
    return(false);
}

int CLuaScriptObject::flagScriptForRemoval()
{ // retVal: 0--> cannot be removed, 1 --> will be removed in a delayed manner, 2--> can be removed now
#ifdef SIM_WITH_GUI
    if (App::mainWindow!=NULL)
        App::mainWindow->scintillaEditorContainer->closeEditor(scriptID);
#endif

    if (App::ct->simulation->isSimulationStopped())
    {
        if ( (_scriptType==sim_scripttype_mainscript)||(_scriptType==sim_scripttype_childscript)||(_scriptType==sim_scripttype_jointctrlcallback)||(_scriptType==sim_scripttype_contactcallback) )//||(_scriptType==sim_scripttype_generalcallback) )
            return(2);
    }
    if (_scriptType==sim_scripttype_customizationscript)
    {
        _flaggedForDestruction=true;
        return(1);
    }
    return(0);
}

int CLuaScriptObject::getObjectIDThatScriptIsAttachedTo_child() const
{
    return(_objectIDAttachedTo_child);
}

int CLuaScriptObject::getObjectIDThatScriptIsAttachedTo_customization() const
{
    return(_objectIDAttachedTo_customization);
}

void CLuaScriptObject::setObjectIDThatScriptIsAttachedTo_child(int newObjectID)
{
    _objectIDAttachedTo_child=newObjectID;
    if (newObjectID!=-1)
        _scriptType=sim_scripttype_childscript;
}

void CLuaScriptObject::setObjectIDThatScriptIsAttachedTo_customization(int newObjectID)
{
    _objectIDAttachedTo_customization=newObjectID;
}

int CLuaScriptObject::getNumberOfPasses() const
{
    return(_numberOfPasses);
}

void CLuaScriptObject::setNumberOfPasses(int p)
{
    _numberOfPasses=p;
}

int CLuaScriptObject::runMainScript(int optionalCallType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    FUNCTION_DEBUG;
    if (_scriptIsDisabled)
        return(sim_script_main_script_not_called); // with this, we do not change simulation state and cannot stop

    int retVal=sim_script_no_error;

    // PUT THESE THINGS SOMEWHERE ELSE (E.G. INTO AN OWN simPrepareSimulationStep)!!!!!!!
    //***********************************************************************
    App::ct->luaScriptContainer->broadcastDataContainer.removeTimedOutObjects(float(App::ct->simulation->getSimulationTime_ns())/1000000.0f); // remove invalid elements

    CThreadPool::prepareAllThreadsForResume_calledBeforeMainScript();

    //***********************************************************************
    if (CThreadPool::getSimulationEmergencyStop())
    {
        CThreadPool::handleAllThreads_withResumeLocation(-1); // Added on 24/02/2011 (important, otherwise threads might never resume!!!)
        return(retVal); // this is important in the case where we pressed the emergency stop (the stop button in the middle of the screen)
    }

    if (_scriptTextExec.size()==0)
    {
        fromFileToBuffer();
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());
    }

    retVal=_runMainScript(optionalCallType,inStack,outStack);

    return(retVal);
}

int CLuaScriptObject::_runMainScript(int optionalCallType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    FUNCTION_DEBUG;
    App::ct->calcInfo->scriptStart(false,true);

    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    int retVal=sim_script_no_error;

    if (optionalCallType==-1)
    {
        if (App::ct->simulation->getSimulationState()==sim_simulation_advancing_firstafterstop)
            retVal|=_runMainScriptNow(sim_syscb_init,inStack,outStack);

        if (_compatibilityModeOrFirstTimeCall_sysCallbacks)
            retVal|=_runMainScriptNow(sim_syscb_actuation,inStack,outStack);
        else
        {
            retVal|=_runMainScriptNow(sim_syscb_actuation,inStack,outStack);
            retVal|=_runMainScriptNow(sim_syscb_sensing,inStack,outStack);
        }

        if (App::ct->simulation->getSimulationState()==sim_simulation_advancing_lastbeforestop)
            retVal|=_runMainScriptNow(sim_syscb_cleanup,inStack,outStack);
    }
    else
    {
        if (!_compatibilityModeOrFirstTimeCall_sysCallbacks)
            retVal|=_runMainScriptNow(optionalCallType,inStack,outStack); // this only supported with the new calling method
    }

    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    App::ct->calcInfo->scriptEnd(false,true);

    // Here we have to prepare the error message for the info box:
    std::string tmpTxt;
    if (retVal&(sim_script_lua_error|sim_script_call_error))
        tmpTxt="&&fg930(script error)";
    App::ct->calcInfo->setMainScriptMessage(tmpTxt.c_str());
    return(retVal);
}

int CLuaScriptObject::_runMainScriptNow(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    std::string errorMsg;
    App::ct->luaScriptContainer->setInMainScriptNow(true,VDateTime::getTimeInMs());
    int retVal=_runScriptOrCallScriptFunction(callType,inStack,outStack,&errorMsg,NULL,NULL);
    App::ct->luaScriptContainer->setInMainScriptNow(false,0);
    if (errorMsg.size()>0)
        _displayScriptError(errorMsg.c_str(),retVal+2); // 0=compilError, 1=runtimeError
    if (retVal<0)
    {
        App::ct->simulation->pauseOnErrorRequested();
        return(sim_script_lua_error);
    }
    return(sim_script_no_error);
}

int CLuaScriptObject::runNonThreadedChildScript(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{ // return value 0 indicates nothing to execute, 1 no error, 2 a lua compilation or runtime error, 3 a lua simulator call error (from this script or its child scripts), 4 if a script is being called several times (reentrance problem)
    FUNCTION_DEBUG;
    int retVal=sim_script_no_error;

    if (_scriptTextExec.size()==0)
    {
        fromFileToBuffer();
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());
    }

    retVal=_runNonThreadedChildScript(callType,inStack,outStack);

    return(retVal);
}

int CLuaScriptObject::_runNonThreadedChildScript(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    FUNCTION_DEBUG;
    App::ct->calcInfo->scriptStart(false,false);

    int retVal=0;
    bool ok=true;
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    if (L==NULL)
    {
        if (callType>sim_syscb_sensing)
            ok=false; // specialized callbacks require the script to be initialized
        if (ok)
            retVal=_runNonThreadedChildScriptNow(sim_syscb_init,NULL,NULL);
    }

    if ( ok&&(callType!=sim_syscb_init) ) // sim_syscb_init was already executed just above!
        retVal=_runNonThreadedChildScriptNow(callType,inStack,outStack);
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)

    App::ct->calcInfo->scriptEnd(false,false);
    return(retVal);
}

int CLuaScriptObject::_runNonThreadedChildScriptNow(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    std::string errorMsg;
    int retVal=_runScriptOrCallScriptFunction(callType,inStack,outStack,&errorMsg,&_containsJointCallbackFunction,&_containsContactCallbackFunction);
    if (errorMsg.size()>0)
        _displayScriptError(errorMsg.c_str(),retVal+2); // 0=compilError, 1=runtimeError
    if (retVal<0)
    {
        App::ct->simulation->pauseOnErrorRequested();
        return(sim_script_lua_error);
    }
    return(sim_script_no_error);
}

int CLuaScriptObject::runThreadedChildScript()
{ // return value 0 indicates nothing to execute, 1 no error, 2 a lua compilation or runtime error, 3 a lua simulator call error (from this script or its child scripts), 4 if a script is being called several times (reentrance problem)
    FUNCTION_DEBUG;
    int retVal=sim_script_no_error;
    if (_threadedExecutionUnderWay)
        return(retVal); // this script is being executed by another thread!

    if (_scriptTextExec.size()==0)
    {
        fromFileToBuffer();
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());
    }

    if (_executeJustOnce&&(_numberOfPasses>0))
        return(0);

    if (CThreadPool::getSimulationStopRequested())// will also return true in case of emergency stop request
        return(retVal);

    _threadedExecutionUnderWay=true;
    _globalMutex.lock();
    toBeCalledByThread.push_back(this);
    _globalMutex.unlock();
    _threadedScript_associatedFiberOrThreadID=CThreadPool::createNewThread(_startAddressForThreadedScripts);
    CThreadPool::switchToThread(_threadedScript_associatedFiberOrThreadID);

    return(retVal);
}

void CLuaScriptObject::_runThreadedChildScriptNow()
{
    /*
    FUNCTION_DEBUG;
    _scriptExecStartTime=VDateTime::getTimeInMs();
    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState(getScriptSuffixNumberString().c_str());
    }
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    tmp+="\nsim_call_type=";
    tmp+=boost::lexical_cast<std::string>((int)-1);
    luaWrap_luaL_dostring(L,tmp.c_str());

    if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
    {
        // Following 6 lines new since 8/11/2014:
        luaWrap_lua_getglobal(L,"debug");
        luaWrap_lua_getfield(L,-1,"traceback");
        luaWrap_lua_remove(L,-2);
        int argCnt=0;
        int errIndex=-argCnt-2;
        luaWrap_lua_insert(L,errIndex);
        if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errIndex)!=0)
        { // a runtime error occurred!
            // We have to exit the thread free mode if we are still in there (the instance should automatically be restored when this thread resumes):
            if (CThreadPool::isThreadInFreeMode())
                CThreadPool::setThreadFreeMode(false);

            std::string errMsg;
            if (luaWrap_lua_isstring(L,-1))
                errMsg=std::string(luaWrap_lua_tostring(L,-1));
            else
                errMsg="(error unknown)";
            luaWrap_lua_pop(L,-1); // pop error from stack

            _displayScriptError(errMsg.c_str(),1);
            // Here we can decide what happens uppon error:
            App::ct->simulation->pauseOnErrorRequested();
        }
        else
        {
            if (CThreadPool::isThreadInFreeMode())
                CThreadPool::setThreadFreeMode(false);
        }
    }
    else
    { // A compilation error occurred!
        std::string errMsg;
        if (luaWrap_lua_isstring(L,-1))
            errMsg=std::string(luaWrap_lua_tostring(L,-1));
        else
            errMsg="(error unknown)";
        _displayScriptError(errMsg.c_str(),0);
        luaWrap_lua_pop(L,-1); // pop error from stack
        // Here we can decide what happens uppon error:
        App::ct->simulation->pauseOnErrorRequested();
    }
    _numberOfPasses++;
    _scriptExecStartTime=-1; // means: not initialized yet
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
*/

    FUNCTION_DEBUG;
    _scriptExecStartTime=VDateTime::getTimeInMs();
//    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState(getScriptSuffixNumberString().c_str());
    }
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    tmp+="\nsim_call_type=-1"; // for backward compatibility
    luaWrap_luaL_dostring(L,tmp.c_str());

    if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
    {
        luaWrap_lua_getglobal(L,"debug");
        luaWrap_lua_getfield(L,-1,"traceback");
        luaWrap_lua_remove(L,-2);
        int argCnt=0;
        int errindex=-argCnt-2;
        luaWrap_lua_insert(L,errindex);
        if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
        { // a runtime error occurred!
            // We have to exit the thread free mode if we are still in there (the instance should automatically be restored when this thread resumes):
            if (CThreadPool::isThreadInFreeMode())
                CThreadPool::setThreadFreeMode(false);

            std::string errMsg;
            if (luaWrap_lua_isstring(L,-1))
                errMsg=std::string(luaWrap_lua_tostring(L,-1));
            else
                errMsg="(error unknown)";
            luaWrap_lua_pop(L,-1); // pop error from stack

            _displayScriptError(errMsg.c_str(),1);
            // Here we can decide what happens uppon error:
            App::ct->simulation->pauseOnErrorRequested();
        }
        else
        {
            int calls[2]={sim_syscb_threadmain,sim_syscb_cleanup};
            bool errOccured=false;
            for (size_t callIndex=0;callIndex<2;callIndex++)
            {
                // Push the function name onto the stack (will be automatically popped from stack after _luaPCall):
                luaWrap_lua_getglobal(L,getSystemCallbackString(calls[callIndex],false).c_str());
                if (luaWrap_lua_isfunction(L,-1))
                { // ok, the function exists!
                    // Push the arguments onto the stack (will be automatically popped from stack after _luaPCall):
                    luaWrap_lua_getglobal(L,"debug");
                    luaWrap_lua_getfield(L,-1,"traceback");
                    luaWrap_lua_remove(L,-2);
                    int argCnt=0;
                    int errindex=-argCnt-2;
                    luaWrap_lua_insert(L,errindex);
                    if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
                    { // a runtime error occurred!
                        if (CThreadPool::isThreadInFreeMode())
                            CThreadPool::setThreadFreeMode(false);
                        std::string errMsg;
                        if (luaWrap_lua_isstring(L,-1))
                            errMsg=std::string(luaWrap_lua_tostring(L,-1));
                        else
                            errMsg="(error unknown)";
                        luaWrap_lua_pop(L,-1); // pop error from stack

                        _displayScriptError(errMsg.c_str(),1);
                        errOccured=true;
                    }
                }
                else
                    luaWrap_lua_pop(L,-1); // pop the function name
            }
            if (CThreadPool::isThreadInFreeMode())
                CThreadPool::setThreadFreeMode(false);
            if (errOccured)
                App::ct->simulation->pauseOnErrorRequested();
        }
    }
    else
    { // A compilation error occurred!
        std::string errMsg;
        if (luaWrap_lua_isstring(L,-1))
            errMsg=std::string(luaWrap_lua_tostring(L,-1));
        else
            errMsg="(error unknown)";
        _displayScriptError(errMsg.c_str(),0);
        luaWrap_lua_pop(L,-1); // pop error from stack
        App::ct->simulation->pauseOnErrorRequested();
    }
    _numberOfPasses++;
    _scriptExecStartTime=-1; // means: not initialized yet
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
//    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
}

bool CLuaScriptObject::runCustomizationScript(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    FUNCTION_DEBUG;
    // Can we run the script? Was it temporarily disabled? Is it allowed to run during simulation? etc. is checked here:
    if (!hasCustomizationScripAnyChanceToGetExecuted(App::ct->simulation->isSimulationRunning(),callType==sim_syscb_cleanup))
        return(false);

    bool retVal=false;
    bool ok=true;
    if (L==NULL)
    {   // The first time we call this script
        if (callType>sim_syscb_sensing)
            ok=false; // specialized callbacks require the script to be initialized
        else
        {
            // Generate the sim_syscb_init call:
            retVal=_runCustomizationScript(sim_syscb_init,inStack,outStack);

            // Above call might have disabled the script. Check again:
            if (!hasCustomizationScripAnyChanceToGetExecuted(App::ct->simulation->isSimulationRunning(),false))
                return(false);

            if (App::ct->simulation->isSimulationPaused())
            {   // Here we have copied an object with customization script into the scene while simulation was paused.
                // We need to generate the sim_syscb_suspend call from here (special case):
                retVal=_runCustomizationScript(sim_syscb_suspend,inStack,outStack);

                // Above call might have disabled the script. Check again:
                if (!hasCustomizationScripAnyChanceToGetExecuted(App::ct->simulation->isSimulationRunning(),false))
                    return(false);
            }
        }
    }

    if ( ok&&(callType!=sim_syscb_init) ) // sim_syscb_init was already handled just here above
        retVal=_runCustomizationScript(callType,inStack,outStack);
    return(retVal);
}

bool CLuaScriptObject::_runCustomizationScript(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    std::string errorMsg;
    int retVal=_runScriptOrCallScriptFunction(callType,inStack,outStack,&errorMsg,&_containsJointCallbackFunction,&_containsContactCallbackFunction);
    if (errorMsg.size()>0)
    {
        errorMsg+=errorWithCustomizationScript(); // might temporarily disable the custom. script
        _displayScriptError(errorMsg.c_str(),retVal+2); // 0=compilError, 1=runtimeError
    }
    return(retVal>0);
}

int CLuaScriptObject::_runScriptOrCallScriptFunction(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack,std::string* errorMsg,bool* hasJointCallbackFunc,bool* hasContactCallbackFunc)
{ // retval: -2: compil error, -1: runtimeError, 0: function not there, 1: ok
    int retVal;
    if (errorMsg!=NULL)
        errorMsg->clear();
    _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    if (_scriptTextExec.size()==0)
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());
    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState(getScriptSuffixNumberString().c_str());
    }
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack
    if (_compatibilityModeOrFirstTimeCall_sysCallbacks)
    {
        std::string tmp("sim_current_script_id=");
        tmp+=boost::lexical_cast<std::string>(getScriptID());
        tmp+="\nsim_call_type="; // for backward compatibility
        tmp+=boost::lexical_cast<std::string>(callType);
        luaWrap_luaL_dostring(L,tmp.c_str());
        if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
        {
            int inputArgs=0;
            if (inStack!=NULL)
            { // for backward compatibility
                inputArgs=inStack->getStackSize();
                if (inputArgs!=0)
                    inStack->buildOntoLuaStack(L,false);
                //inStack->clear();
            }
            luaWrap_lua_getglobal(L,"debug");
            luaWrap_lua_getfield(L,-1,"traceback");
            luaWrap_lua_remove(L,-2);
            int argCnt=inputArgs;
            int errindex=-argCnt-2;
            luaWrap_lua_insert(L,errindex);
            if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
            { // a runtime error occurred!
                if (errorMsg!=NULL)
                {
                    if (luaWrap_lua_isstring(L,-1))
                        errorMsg[0]=std::string(luaWrap_lua_tostring(L,-1));
                    else
                        errorMsg[0]="(error unknown)";
                }
                luaWrap_lua_pop(L,-1); // pop error from stack
                retVal=-1;
            }
            else
            {
                if (callType==sim_syscb_init)
                { // here we check if we can enable the new calling method:
                    std::string initCb=getSystemCallbackString(sim_syscb_init,false);
                    luaWrap_lua_getglobal(L,initCb.c_str());
                    _compatibilityModeOrFirstTimeCall_sysCallbacks=!(luaWrap_lua_isfunction(L,-1));
                    luaWrap_lua_pop(L,-1);
                    if (!_compatibilityModeOrFirstTimeCall_sysCallbacks)
                        luaWrap_luaL_dostring(L,"sim_call_type=nil");
                }
                else
                { // for backward compatibility:
                    int currentTop=luaWrap_lua_gettop(L);
                    int numberOfArgs=currentTop-oldTop-1; // the first arg is linked to the debug mechanism
                    if (outStack!=NULL)
                        outStack->buildFromLuaStack(L,oldTop+1+1,numberOfArgs); // the first arg is linked to the debug mechanism
                }
                retVal=1;
            }
            _numberOfPasses++;
        }
        else
        { // A compilation error occurred!
            if (errorMsg!=NULL)
            {
                if (luaWrap_lua_isstring(L,-1))
                    errorMsg[0]=std::string(luaWrap_lua_tostring(L,-1));
                else
                    errorMsg[0]="(error unknown)";
            }
            luaWrap_lua_pop(L,-1); // pop error from stack
            retVal=-2;
        }
    }
    if (!_compatibilityModeOrFirstTimeCall_sysCallbacks)
    {
        if (callType==sim_syscb_init)
        { // do this only once
            luaWrap_lua_getglobal(L,getSystemCallbackString(sim_syscb_jointcallback,false).c_str());
            if (hasJointCallbackFunc!=NULL)
                hasJointCallbackFunc[0]=luaWrap_lua_isfunction(L,-1);
            luaWrap_lua_getglobal(L,getSystemCallbackString(sim_syscb_contactcallback,false).c_str());
            if (hasContactCallbackFunc!=NULL)
                hasContactCallbackFunc[0]=luaWrap_lua_isfunction(L,-1);
            luaWrap_lua_pop(L,2);
        }
        // Push the function name onto the stack (will be automatically popped from stack after _luaPCall):
        luaWrap_lua_getglobal(L,getSystemCallbackString(callType,false).c_str());
        if (luaWrap_lua_isfunction(L,-1))
        { // ok, the function exists!
            // Push the arguments onto the stack (will be automatically popped from stack after _luaPCall):
            int inputArgs=0;
            if (inStack!=NULL)
            {
                inputArgs=inStack->getStackSize();
                if (inputArgs!=0)
                    inStack->buildOntoLuaStack(L,false);
                //inStack->clear();
            }
            luaWrap_lua_getglobal(L,"debug");
            luaWrap_lua_getfield(L,-1,"traceback");
            luaWrap_lua_remove(L,-2);
            int argCnt=inputArgs;
            int errindex=-argCnt-2;
            luaWrap_lua_insert(L,errindex);
            if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
            { // a runtime error occurred!
                if (errorMsg!=NULL)
                {
                    if (luaWrap_lua_isstring(L,-1))
                        errorMsg[0]=std::string(luaWrap_lua_tostring(L,-1));
                    else
                        errorMsg[0]="(error unknown)";
                }
                luaWrap_lua_pop(L,-1); // pop error from stack
                retVal=-1;
            }
            else
            { // return values:
                int currentTop=luaWrap_lua_gettop(L);
                int numberOfArgs=currentTop-oldTop-1; // the first arg is linked to the debug mechanism
                if (outStack!=NULL)
                    outStack->buildFromLuaStack(L,oldTop+1+1,numberOfArgs); // the first arg is linked to the debug mechanism
                retVal=1;
            }
        }
        else
        {
            luaWrap_lua_pop(L,-1); // pop the function name
            retVal=0;
        }
    }
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    _scriptExecStartTime=-1; // means: not initialized yet
    return(retVal);
}

void CLuaScriptObject::runAddOn(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    if (_flaggedForDestruction)
        callType=sim_syscb_cleanup;
    if (L==NULL)
    {
        if ( (callType==sim_syscb_init)||(_addOn_executionState==-1) ) // second arg. is for auto-run
        {
            if ( (_addOn_executionState!=-1)||(callType==sim_syscb_init)||(callType==sim_syscb_aos_run) )
                _runAddOn(sim_syscb_init,inStack,outStack);
        }
    }
    else
    { // ok, the add-on was already initialized. We can run it, suspend it, restart it, or kill it (and a few others):
        if (callType==sim_syscb_aos_run)
        {
            if (_addOn_executionState!=sim_syscb_aos_suspend) // when suspended, we first need to unsuspend it
                _runAddOn(callType,inStack,outStack);
        }
        else if (callType==sim_syscb_aos_suspend)
        {
            if (_addOn_executionState==sim_syscb_aos_run) // only when running
                _runAddOn(callType,inStack,outStack);
        }
        else if (callType==sim_syscb_aos_resume)
        {
            if (_addOn_executionState==sim_syscb_aos_suspend) // only when suspended
                _runAddOn(callType,inStack,outStack);
        }
        else if (callType==sim_syscb_cleanup)
            killLuaState();
        else
        {
            if (_addOn_executionState!=sim_syscb_aos_suspend) // when suspended, we don't handle the other system calls
                _runAddOn(callType,inStack,outStack);
        }
    }
}

void CLuaScriptObject::_runAddOn(int callType,const CInterfaceStack* inStack,CInterfaceStack* outStack)
{
    std::string errorMsg;
    CInterfaceStack outStackTmp;
    CInterfaceStack* outStackProxy;
    if (outStack!=NULL)
        outStackProxy=outStack;
    else
        outStackProxy=&outStackTmp;

    int retVal;
    if (_compatibilityModeOrFirstTimeCall_sysCallbacks)
    { // for backward compatibility
        CInterfaceStack inStackLocal;
        inStackLocal.pushNumberOntoStack(double(callType));
        retVal=_runScriptOrCallScriptFunction(callType,&inStackLocal,outStackProxy,&errorMsg,NULL,NULL);
    }
    else
        retVal=_runScriptOrCallScriptFunction(callType,inStack,outStackProxy,&errorMsg,NULL,NULL);
    if (retVal>-2)
    {
        if ( (callType==sim_syscb_init)||(callType==sim_syscb_cleanup)||(callType==sim_syscb_aos_run)||(callType==sim_syscb_aos_suspend)||(callType==sim_syscb_aos_resume) )
            _addOn_executionState=callType;
    }
    if (errorMsg.size()>0)
        _displayScriptError(errorMsg.c_str(),retVal+2); // 0=compilError, 1=runtimeError
    if ( (outStackProxy->getStackSize()>0)&&(callType!=sim_syscb_cleanup) )
    {
        CInterfaceStack* outStackCopy=outStackProxy->copyYourself();
        outStackCopy->moveStackItemToTop(0);
        double retNb;
        if (outStackCopy->getStackNumberValue(retNb))
        {
            int r=int(retNb+0.01);
            if ( (r==sim_syscb_cleanup)||(r==sim_syscb_init) ) // second part is for backward compatibility
                killLuaState(); // we want to run the clean-up procedure
        }
        delete outStackCopy;
    }
}

VTHREAD_ID_TYPE CLuaScriptObject::getThreadedScriptThreadId() const
{
    return(_threadedScript_associatedFiberOrThreadID);
}

VTHREAD_RETURN_TYPE CLuaScriptObject::_startAddressForThreadedScripts(VTHREAD_ARGUMENT_TYPE lpData)
{
    FUNCTION_DEBUG;
    _globalMutex.lock();
    CLuaScriptObject* it=toBeCalledByThread[0];
    toBeCalledByThread.erase(toBeCalledByThread.begin());
    _globalMutex.unlock();

    App::ct->calcInfo->scriptStart(true,false);

    CApiErrors::addNewThreadForErrorReporting(it->getScriptID()); // for correct error handling (i.e. assignement to the correct script and output)
    it->_runThreadedChildScriptNow();
    CApiErrors::removeThreadFromErrorReporting(); // for correct error handling (i.e. assignement to the correct script and output)

    App::ct->calcInfo->scriptEnd(true,false);

    it->killLuaState(); // added on 2010/03/05 because of some crashes with threaded scripts (Lua said: resource destroyed by other thread than the one that created it!)
    it->_threadedExecutionUnderWay=false;

    return(VTHREAD_RETURN_VAL);
}

int CLuaScriptObject::callScriptFunction(const char* functionName,SLuaCallBack* pdata)
{
    int retVal=-1; // means error

    if (!_prepareLuaStateAndCallScriptInitSectionIfNeeded())
        return(retVal);

    bool setAndResetScriptExecStartTime=(_scriptExecStartTime==-1);
    if (setAndResetScriptExecStartTime)
        _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    // New since 7/3/2016:
    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    tmp+="\nsim_call_type=-1";
    luaWrap_luaL_dostring(L,tmp.c_str());

    // Push the function name onto the stack (will be automatically popped from stack after _luaPCall):
    luaWrap_lua_getglobal(L,functionName);

    // Push the arguments onto the stack (will be automatically popped from stack after _luaPCall):
    int inputArgs=pdata->inputArgCount;
    if (inputArgs!=0)
    {
        int boolPt=0;
        int intPt=0;
        int floatPt=0;
        int doublePt=0;
        int stringPt=0;
        int stringBuffPt=0;
        for (int i=0;i<pdata->inputArgCount;i++)
            writeCustomFunctionDataOntoStack(L,pdata->inputArgTypeAndSize[2*i+0],pdata->inputArgTypeAndSize[2*i+1],pdata->inputBool,boolPt,pdata->inputInt,intPt,pdata->inputFloat,floatPt,pdata->inputDouble,doublePt,pdata->inputChar,stringPt,pdata->inputCharBuff,stringBuffPt);
    }

    luaWrap_lua_getglobal(L,"debug");
    luaWrap_lua_getfield(L,-1,"traceback");
    luaWrap_lua_remove(L,-2);
    int argCnt=inputArgs;
    int errindex=-argCnt-2;
    luaWrap_lua_insert(L,errindex);

    // Following line new since 7/3/2016:
    if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
    { // a runtime error occurred!
        std::string errMsg;
        if (luaWrap_lua_isstring(L,-1))
            errMsg=std::string(luaWrap_lua_tostring(L,-1));
        else
            errMsg="(error unknown)";
        luaWrap_lua_pop(L,-1); // pop error from stack
        _displayScriptError(errMsg.c_str(),1);

        // Following probably not needed:
        pdata->outputBool=new unsigned char[0];
        pdata->outputInt=new int[0];
        pdata->outputFloat=new float[0];
        pdata->outputDouble=new double[0];
        pdata->outputChar=new char[0];
        pdata->outputCharBuff=new char[0];
    }
    else
    { // return values:
        int currentTop=luaWrap_lua_gettop(L);

        // Following line new since 7/3/2016:
        int numberOfArgs=currentTop-oldTop-1; // the first arg is linked to the debug mechanism

        // We read all arguments from the stack
        std::vector<char> outBoolVector;
        std::vector<int> outIntVector;
        std::vector<float> outFloatVector;
        std::vector<double> outDoubleVector;
        std::vector<std::string> outStringVector;
        std::vector<std::string> outCharVector;
        std::vector<int> outInfoVector;
        for (int i=0;i<numberOfArgs;i++)
        {
            // Following line new since 7/3/2016:
            if (!readCustomFunctionDataFromStack(L,oldTop+i+1+1,pdata->outputArgTypeAndSize[i*2+0],outBoolVector,outIntVector,outFloatVector,outDoubleVector,outStringVector,outCharVector,outInfoVector))
                break;
        }

        // Now we prepare the output buffers:
        pdata->outputBool=new unsigned char[outBoolVector.size()];
        pdata->outputInt=new int[outIntVector.size()];
        pdata->outputFloat=new float[outFloatVector.size()];
        pdata->outputDouble=new double[outDoubleVector.size()];
        int charCnt=0;
        for (size_t k=0;k<outStringVector.size();k++)
            charCnt+=(int)outStringVector[k].length()+1; // terminal 0
        pdata->outputChar=new char[charCnt];

        int charBuffCnt=0;
        for (size_t k=0;k<outCharVector.size();k++)
            charBuffCnt+=(int)outCharVector[k].length();
        pdata->outputCharBuff=new char[charBuffCnt];

        pdata->outputArgCount=int(outInfoVector.size()/2);
        delete[] pdata->outputArgTypeAndSize;
        pdata->outputArgTypeAndSize=new int[outInfoVector.size()];
        // We fill the output buffers:
        for (int k=0;k<int(outBoolVector.size());k++)
            pdata->outputBool[k]=outBoolVector[k];
        for (int k=0;k<int(outIntVector.size());k++)
            pdata->outputInt[k]=outIntVector[k];
        for (int k=0;k<int(outFloatVector.size());k++)
            pdata->outputFloat[k]=outFloatVector[k];
        for (int k=0;k<int(outDoubleVector.size());k++)
            pdata->outputDouble[k]=outDoubleVector[k];
        charCnt=0;
        for (size_t k=0;k<outStringVector.size();k++)
        {
            for (size_t l=0;l<outStringVector[k].length();l++)
                pdata->outputChar[charCnt+l]=outStringVector[k][l];
            charCnt+=(int)outStringVector[k].length();
            // terminal 0:
            pdata->outputChar[charCnt]=0;
            charCnt++;
        }

        charBuffCnt=0;
        for (size_t k=0;k<outCharVector.size();k++)
        {
            for (size_t l=0;l<outCharVector[k].length();l++)
                pdata->outputCharBuff[charBuffCnt+l]=outCharVector[k][l];
            charBuffCnt+=(int)outCharVector[k].length();
        }

        for (int k=0;k<int(outInfoVector.size());k++)
            pdata->outputArgTypeAndSize[k]=outInfoVector[k];

        retVal=0;
    }
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    if (setAndResetScriptExecStartTime)
        _scriptExecStartTime=-1; // means: not initialized yet
    return(retVal);
}

int CLuaScriptObject::callScriptFunctionEx(const char* functionName,CInterfaceStack* stack)
{ // retVal: -3: could not initialize script, -2: function does not exist, -1: error in function, 0:ok
    int retVal=-3;

    if (!_prepareLuaStateAndCallScriptInitSectionIfNeeded())
        return(retVal);
    retVal=-2;

    bool setAndResetScriptExecStartTime=(_scriptExecStartTime==-1);
    if (setAndResetScriptExecStartTime)
        _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    tmp+="\nsim_call_type=-1";
    luaWrap_luaL_dostring(L,tmp.c_str());

    // Push the function name onto the stack (will be automatically popped from stack after _luaPCall):
    luaWrap_lua_getglobal(L,functionName);
    if (luaWrap_lua_isfunction(L,-1))
    {
        retVal=-1;
        // Push the arguments onto the stack (will be automatically popped from stack after _luaPCall):
        int inputArgs=stack->getStackSize();

        if (inputArgs!=0)
            stack->buildOntoLuaStack(L,false);

        stack->clear();

        luaWrap_lua_getglobal(L,"debug");
        luaWrap_lua_getfield(L,-1,"traceback");
        luaWrap_lua_remove(L,-2);
        int argCnt=inputArgs;
        int errindex=-argCnt-2;
        luaWrap_lua_insert(L,errindex);

        if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errindex)!=0)
        { // a runtime error occurred!
            std::string errMsg;
            if (luaWrap_lua_isstring(L,-1))
                errMsg=std::string(luaWrap_lua_tostring(L,-1));
            else
                errMsg="(error unknown)";
            luaWrap_lua_pop(L,-1); // pop error from stack
            if (_errorReportMode!=0)
                _displayScriptError(errMsg.c_str(),1);
        }
        else
        { // return values:
            int currentTop=luaWrap_lua_gettop(L);

            int numberOfArgs=currentTop-oldTop-1; // the first arg is linked to the debug mechanism
            stack->buildFromLuaStack(L,oldTop+1+1,numberOfArgs); // the first arg is linked to the debug mechanism
            retVal=0;
        }
    }
    else
        luaWrap_lua_pop(L,-1); // pop the function name
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    if (setAndResetScriptExecStartTime)
        _scriptExecStartTime=-1; // means: not initialized yet
    return(retVal);
}

int CLuaScriptObject::setScriptVariable(const char* variableName,CInterfaceStack* stack)
{
    if (!_prepareLuaStateAndCallScriptInitSectionIfNeeded())
        return(-1);

    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    if (stack!=NULL)
        stack->buildOntoLuaStack(L,true);
    else
        luaWrap_lua_pushnil(L);
    luaWrap_lua_setglobal(L,variableName);

    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    return(0);
}

int CLuaScriptObject::clearVariable(const char* variableName)
{
    if (L==NULL)
        return(-1);

    if (_threadedExecution)
    {
        if (!VThread::areThreadIDsSame(_threadedScript_associatedFiberOrThreadID,VThread::getCurrentThreadId()))
            return(-1); // only from the same thread when threaded!!
    }

    if (_scriptIsDisabled)
        return(-1);


    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    luaWrap_lua_pushnil(L);
    luaWrap_lua_setglobal(L,variableName);

    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    return(0);
}

int CLuaScriptObject::executeScriptString(const char* scriptString,CInterfaceStack* stack)
{
    int retVal=-1;
    if (!_prepareLuaStateAndCallScriptInitSectionIfNeeded())
        return(retVal);

    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)

//    std::string tmp("sim_current_script_id=");

//    CApiErrors::
    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string theString("return ");
    theString+=scriptString;
    if (0!=luaWrap_luaL_dostring(L,theString.c_str()))
    { // 'return theStringToExecute' failed to execute. Let's simply execute 'theStringToExecute'
        int intermediateTop=luaWrap_lua_gettop(L);
        if (0==luaWrap_luaL_dostring(L,scriptString))
            retVal=0; // ok, that worked.
        // Now build the return value or error onto the stack

        if (stack!=NULL)
        {
            stack->clear();
            if (luaWrap_lua_gettop(L)>intermediateTop)
                stack->buildFromLuaStack(L,intermediateTop+1,1);
        }
    }
    else
    { // ok, let's see what we have in return:
        if (stack!=NULL)
        {
            stack->clear();
            if (luaWrap_lua_gettop(L)>oldTop)
                stack->buildFromLuaStack(L,oldTop+1,1);
        }
        retVal=0;
    }

    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    return(retVal);
}

bool CLuaScriptObject::hasLuaState() const
{
    return(L!=NULL);
}

void CLuaScriptObject::killLuaState()
{
    if (L!=NULL)
    {
        CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)
        if (_scriptType==sim_scripttype_customizationscript)
        {
            if (hasCustomizationScripAnyChanceToGetExecuted(false,true)) // run the last pass even if we do not wish to execute the script during simulation
                _runCustomizationScript(sim_syscb_cleanup,NULL,NULL);
        }

        if (_scriptType==sim_scripttype_mainscript)
        {
            // Not needed for the main script (was already handled elsewhere!)
        }

        if (_scriptType==sim_scripttype_addonfunction)
        {
            // Not needed
        }

        if (_scriptType==sim_scripttype_sandboxscript)
        {
            // Not needed
        }
        if (_scriptType==sim_scripttype_addonscript)
        {
            _runAddOn(sim_syscb_cleanup,NULL,NULL);
        }

        if (_scriptType==sim_scripttype_jointctrlcallback)
        {
            std::vector<bool> inDataBool;
            std::vector<int> inDataInt;
            std::vector<float> inDataFloat;
            std::vector<float> outData;
            std::vector<bool> outDataValidity;
            _runJointCtrlCallback_OLD(sim_syscb_cleanup,inDataBool,inDataInt,inDataFloat,outData,outDataValidity);
        }

        if ((_scriptType==sim_scripttype_childscript)&&(!_threadedExecution) )
        {
            // Following few not elegant, but important otherwise we don't have the emergency stop button
            bool wasInMainScript=App::ct->luaScriptContainer->getInMainScriptNow();
            if (!wasInMainScript)
                App::ct->luaScriptContainer->setInMainScriptNow(true,VDateTime::getTimeInMs());
            _runNonThreadedChildScriptNow(sim_syscb_cleanup,NULL,NULL);
            if (!wasInMainScript)
                App::ct->luaScriptContainer->setInMainScriptNow(false,0);
        }
        CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
        luaWrap_lua_close(L);
    }
    L=NULL;
    _loadBufferResult=-1;
    _scriptTextExec.clear();
    if (!_threadedExecution)
        _numberOfPasses=0;
    _addOn_executionState=sim_syscb_init;
    _customizationScriptIsTemporarilyDisabled=false;
    _compatibilityModeOrFirstTimeCall_sysCallbacks=true;
    _containsJointCallbackFunction=false;
    _containsContactCallbackFunction=false;
    _flaggedForDestruction=false;
}

int CLuaScriptObject::setUserData(char* data)
{
    _userData.push_back(data);
    int id=0;
    for (int i=0;i<int(_userDataIds.size());i++)
    {
        if (_userDataIds[i]==id)
        {
            id++;
            i=-1; // we need to restart the search from the beginning!
        }
    }
    _userDataIds.push_back(id);
    return(id);
}

char* CLuaScriptObject::getUserData(int id) const
{
    for (size_t i=0;i<_userDataIds.size();i++)
    {
        if (_userDataIds[i]==id)
            return(_userData[i]);
    }
    return(NULL);
}

void CLuaScriptObject::releaseUserData(int id)
{
    for (int i=0;i<int(_userDataIds.size());i++)
    {
        if (_userDataIds[i]==id)
        {
            delete[] _userData[i];
            _userData.erase(_userData.begin()+i);
            _userDataIds.erase(_userDataIds.begin()+i);
            break;
        }
    }
}

void CLuaScriptObject::clearAllUserData()
{
    for (int i=0;i<int(_userData.size());i++)
        delete[] _userData[i];
    _userData.clear();
    _userDataIds.clear();
}

CLuaScriptObject* CLuaScriptObject::copyYourself()
{
    CLuaScriptObject* it=new CLuaScriptObject(_scriptType);
    it->scriptID=scriptID;
    it->_objectIDAttachedTo_child=_objectIDAttachedTo_child;
    it->_objectIDAttachedTo_callback=_objectIDAttachedTo_callback;
    it->_objectIDAttachedTo_customization=_objectIDAttachedTo_customization;
    it->_threadedExecution=_threadedExecution;
    it->_scriptIsDisabled=_scriptIsDisabled;
    it->_executionOrder=_executionOrder;
    it->_customizationScriptIsTemporarilyDisabled=_customizationScriptIsTemporarilyDisabled;
    std::vector<int> foldingThing;
    it->setScriptText(getScriptText(&foldingThing),&foldingThing);

    delete it->scriptParameters;
    it->scriptParameters=scriptParameters->copyYourself();
    it->_executeJustOnce=_executeJustOnce;

    delete it->_customObjectData;
    it->_customObjectData=NULL;
    if (_customObjectData!=NULL)
        it->_customObjectData=_customObjectData->copyYourself();

    delete it->_customObjectData_tempData;
    it->_customObjectData_tempData=NULL;
    if (_customObjectData_tempData!=NULL)
        it->_customObjectData_tempData=_customObjectData_tempData->copyYourself();

    it->_initialValuesInitialized=_initialValuesInitialized;
    return(it);
}

CLuaScriptParameters* CLuaScriptObject::getScriptParametersObject()
{
    return(scriptParameters);
}

bool CLuaScriptObject::addCommandToOutsideCommandQueue(int commandID,int auxVal1,int auxVal2,int auxVal3,int auxVal4,const float aux2Vals[8],int aux2Count)
{
    if (_outsideCommandQueue!=NULL)
        return(_outsideCommandQueue->addCommand(commandID,auxVal1,auxVal2,auxVal3,auxVal4,aux2Vals,aux2Count));
    return(true);
}

int CLuaScriptObject::extractCommandFromOutsideCommandQueue(int auxVals[4],float aux2Vals[8],int& aux2Count)
{
    if (_outsideCommandQueue!=NULL)
        return(_outsideCommandQueue->extractOneCommand(auxVals,aux2Vals,aux2Count));
    return(-1);
}

bool CLuaScriptObject::_prepareLuaStateAndCallScriptInitSectionIfNeeded()
{
    if ( (L==NULL)&&_threadedExecution )
        return(false); // threaded scripts must be called from within, or already initialized!

    if (_threadedExecution)
    {
        if (!VThread::areThreadIDsSame(_threadedScript_associatedFiberOrThreadID,VThread::getCurrentThreadId()))
            return(false); // only from the same thread when threaded!!
    }

    if (_scriptIsDisabled)
        return(false);

    if (L!=NULL)
        return(true);

    // this script was never run before!
    if (_scriptType==sim_scripttype_sandboxscript)
    {
        // not needed
    }
    if (_scriptType==sim_scripttype_generalcallback)
        runGeneralCallback_OLD(-1,0,NULL); // dummy call
    if (_scriptType==sim_scripttype_customizationscript)
        runCustomizationScript(sim_syscb_init,NULL,NULL); // dummy call
    if (!App::ct->simulation->isSimulationStopped())
    { // following only when simulation is running!
        if (_scriptType==sim_scripttype_contactcallback)
        {
            int in[3]={0,0,-1};
            int out1[3];
            float out2[14];
            runContactCallback_OLD(in,out1,out2); // dummy call
        }
        if (_scriptType==sim_scripttype_childscript)
            runNonThreadedChildScript(sim_syscb_init,NULL,NULL); // dummy call
        if (_scriptType==sim_scripttype_jointctrlcallback)
        {
            std::vector<bool> inDataBool;
            std::vector<int> inDataInt;
            std::vector<float> inDataFloat;
            std::vector<float> outData;
            std::vector<bool> outDataValidity;
            inDataBool.push_back(true); // init
            inDataBool.push_back(true); // revolute
            inDataBool.push_back(true); // cyclic
            inDataInt.push_back(-1); // joint ID
            inDataInt.push_back(0); // pass count
            inDataInt.push_back(0); // total passes
            inDataFloat.push_back(0.0f); // current pos
            inDataFloat.push_back(0.0f); // target pos
            inDataFloat.push_back(0.0f); // error v
            inDataFloat.push_back(0.0f); // effort
            inDataFloat.push_back(0.0f); // dyn step size
            inDataFloat.push_back(0.0f); // low limit
            inDataFloat.push_back(0.0f); // high limit
            inDataFloat.push_back(0.0f); // target vel
            inDataFloat.push_back(0.0f); // max force
            inDataFloat.push_back(0.0f); // upper vel limit
            runJointCtrlCallback_OLD(inDataBool,inDataInt,inDataFloat,outData,outDataValidity); // dummy call
        }
    }
    return(L!=NULL);
}

void CLuaScriptObject::_displayScriptError(const char* errMsg,int errorType)
{
    std::string message("Lua unknown error: ");
    if (errorType==0)
        message="Lua compilation error: ";
    if (errorType==1)
    {
        std::string err(errMsg);
        if (err.find("attempt to yield across metamethod/C-call boundary")!=std::string::npos)
            return; // silent error when breaking out of a threaded child script at simulation end
        message="Lua runtime error: ";
    }
    if (errorType==2)
        message="System callback error: ";
    message+=errMsg;
    App::addStatusbarMessage(message.c_str());
    SUIThreadCommand cmdIn;
    SUIThreadCommand cmdOut;
    cmdIn.cmdId=FLASH_STATUSBAR_UITHREADCMD;
    App::uiThread->executeCommandViaUiThread(&cmdIn,&cmdOut);
}


void CLuaScriptObject::serialize(CSer& ar)
{
    if (ar.isStoring())
    {       // Storing
        ar.storeDataName("Si2");
        ar << scriptID << _objectIDAttachedTo_child << _scriptType;
        ar.flush();

        // Keep following close to the beginning!
        ar.storeDataName("Va2");
        unsigned char nothing=0;
        SIM_SET_CLEAR_BIT(nothing,0,_threadedExecution);
        SIM_SET_CLEAR_BIT(nothing,1,_scriptIsDisabled);
        // RESERVED
        SIM_SET_CLEAR_BIT(nothing,3,!_mainScriptIsDefaultMainScript);
        SIM_SET_CLEAR_BIT(nothing,4,_executeJustOnce);
        // RESERVED!!
        SIM_SET_CLEAR_BIT(nothing,6,true); // this indicates we have the 'almost' new script execution engine (since V3.1.3)
        SIM_SET_CLEAR_BIT(nothing,7,true); // this indicates we have the new script execution engine (since V3.1.3)
        ar << nothing;
        ar.flush();

        // Keep following close to the beginning!
        ar.storeDataName("Va3"); // this is also used as a marked for the color correction introduced in V3.1.4 and later!
        nothing=0;
        SIM_SET_CLEAR_BIT(nothing,0,true); // needed for a code correction
        SIM_SET_CLEAR_BIT(nothing,1,!_disableCustomizationScriptWithError);
        ar << nothing;
        ar.flush();

        ar.storeDataName("Seo");
        ar << _executionOrder;
        ar.flush();


        std::string stt(_scriptText);
        bool storeAlsoFoldingInfo=true;
        if (!App::ct->undoBufferContainer->isUndoSavingOrRestoringUnderWay())
        { // normal serialization (i.e. no undo/redo serialization):
            if (App::ct->luaScriptContainer->getSaveIncludeScriptFiles())
            {
                std::string filename(getIncludeScriptFilePathAndName());
                if (VFile::doesFileExist(filename))
                {
                    storeAlsoFoldingInfo=false;
                    try
                    {
                        VFile file(filename,VFile::READ|VFile::SHARE_DENY_NONE);
                        VArchive archive(&file,VArchive::LOAD);
                        stt.clear();
                        size_t archiveLength=(size_t)file.getLength();
                        char chr;
                        for (size_t fl=0;fl<archiveLength;fl++)
                        {
                            archive >> chr;
                            stt.push_back(chr);
                        }
                        stt.push_back('\0');
                        archive.close();
                        file.close();
                    }
                    catch(VFILE_EXCEPTION_TYPE e)
                    {
                    }
                }
            }
        }

        // We store scripts in a light encoded way:
        ar.storeDataName("Ste");
        tt::lightEncodeBuffer(&stt[0],int(stt.length()));
        for (size_t i=0;i<stt.length();i++)
            ar << stt[i];
        ar.flush();

        if (storeAlsoFoldingInfo)
        {
            ar.storeDataName("Sfi");
            ar << int(_scriptFoldingInfo.size());
            for (int i=0;i<int(_scriptFoldingInfo.size());i++)
                ar << _scriptFoldingInfo[i];
            ar.flush();
        }


        ar.storeDataName("Prm");
        ar.setCountingMode();
        scriptParameters->serialize(ar);
        if (ar.setWritingMode())
            scriptParameters->serialize(ar);

        ar.storeDataName("Coi");
        ar << _objectIDAttachedTo_callback;
        ar.flush();

        ar.storeDataName("Coc");
        ar << _objectIDAttachedTo_customization;
        ar.flush();

        if (_customObjectData!=NULL)
        {
            ar.storeDataName("Cod");
            ar.setCountingMode();
            _customObjectData->serializeData(ar);
            if (ar.setWritingMode())
                _customObjectData->serializeData(ar);
        }

        ar.storeDataName(SER_END_OF_OBJECT);
    }
    else
    {       // Loading
        int byteQuantity;
        std::string theName="";
        bool backwardCompatibility_7_8_2014=false;
        bool backwardCompatibility_13_8_2014=false;
        bool executeInSensingPhase_oldCompatibility_7_8_2014=false;
        bool backwardCompatibilityCorrectionNeeded_13_10_2014=true;
        bool backwardCompatibilityCorrectionNeeded_8_11_2014=true;
        while (theName.compare(SER_END_OF_OBJECT)!=0)
        {
            theName=ar.readDataName();
            if (theName.compare(SER_END_OF_OBJECT)!=0)
            {
                bool noHit=true;
                bool justLoadedCustomScriptBuffer=false;
                if (theName.compare("Si2")==0)
                {
                    noHit=false;
                    ar >> byteQuantity;
                    ar >> scriptID >> _objectIDAttachedTo_child >> _scriptType;
                }
                if (theName.compare("Seo")==0)
                { 
                    noHit=false;
                    ar >> byteQuantity;
                    ar >> _executionOrder;
                }

                if (theName.compare("Sfi")==0)
                {
                    noHit=false;
                    ar >> byteQuantity;
                    int q;
                    ar >> q;
                    _scriptFoldingInfo.resize(q);
                    for (int i=0;i<q;i++)
                        ar >> _scriptFoldingInfo[i];
                }

                if (theName.compare("Ste")==0)
                { // The script is stored encoded!
                    noHit=false;
                    ar >> byteQuantity;

                    _scriptText.resize(byteQuantity,' ');
                    if (byteQuantity!=0)
                    {
                        for (int i=0;i<byteQuantity;i++)
                            ar >> _scriptText[i];
                        tt::lightDecodeBuffer(&_scriptText[0],byteQuantity);
                        while ( (_scriptText.size()>0)&&(_scriptText[_scriptText.size()-1]==0) )
                            _scriptText.erase(_scriptText.end()-1); // to fix a compatibility bug
                    }
                    justLoadedCustomScriptBuffer=true;
                }
                if (justLoadedCustomScriptBuffer)
                { // We just loaded the script text.
                    if (_scriptType==sim_scripttype_mainscript)
                    { // We just loaded a main script text. Do we want to load the default main script instead?
                        if (_mainScriptIsDefaultMainScript)
                        { // Yes!
                            std::string filenameAndPath(App::directories->systemDirectory+VREP_SLASH);
                            filenameAndPath+=DEFAULT_MAINSCRIPT_NAME;
                            if (VFile::doesFileExist(filenameAndPath))
                            {
                                try
                                {
                                    VFile file(filenameAndPath.c_str(),VFile::READ|VFile::SHARE_DENY_NONE);
                                    VArchive archive2(&file,VArchive::LOAD);
                                    unsigned int archiveLength=(unsigned int)file.getLength();
                                    // We replace current script with a default main script
                                    _scriptText.resize(archiveLength,' ');
                                    for (unsigned int i=0;i<archiveLength;i++)
                                        archive2 >> _scriptText[i];
                                    archive2.close();
                                    file.close();
                                }
                                catch(VFILE_EXCEPTION_TYPE e)
                                {
                                    VFile::reportAndHandleFileExceptionError(e);
                                    // Removed following line on 2010/03/03: even if the default main script is not there, we might still want the default main script next time (if there maybe).
                                    // Following line also causes problems when converting to a new fileformat!
                                    // _mainScriptIsDefaultMainScript=false; // We couldn't find the default main script, we turn this one into a customized main script!
                                }
                            }
                        }
                    }
                }
                if (theName=="Va2")
                {
                    noHit=false;
                    ar >> byteQuantity;
                    unsigned char nothing;
                    ar >> nothing;
                    _threadedExecution=SIM_IS_BIT_SET(nothing,0);
                    _scriptIsDisabled=SIM_IS_BIT_SET(nothing,1);
                    // RESERVED
                    _mainScriptIsDefaultMainScript=!SIM_IS_BIT_SET(nothing,3);
                    _executeJustOnce=SIM_IS_BIT_SET(nothing,4);
                    executeInSensingPhase_oldCompatibility_7_8_2014=SIM_IS_BIT_SET(nothing,5);
                    backwardCompatibility_7_8_2014=!SIM_IS_BIT_SET(nothing,6);
                    backwardCompatibility_13_8_2014=!SIM_IS_BIT_SET(nothing,7);
                }
                if (theName=="Va3")
                {
                    noHit=false;
                    ar >> byteQuantity;
                    unsigned char nothing;
                    ar >> nothing;
                    backwardCompatibilityCorrectionNeeded_8_11_2014=!SIM_IS_BIT_SET(nothing,0);
                    backwardCompatibilityCorrectionNeeded_13_10_2014=false;
                    _disableCustomizationScriptWithError=!SIM_IS_BIT_SET(nothing,1);

                }
                if (theName.compare("Prm")==0)
                {
                    noHit=false;
                    ar >> byteQuantity; // never use that info, unless loading unknown data!!!! (undo/redo stores dummy info in there)
                    delete scriptParameters;
                    scriptParameters=new CLuaScriptParameters();
                    scriptParameters->serialize(ar);
                    if (_scriptType==sim_scripttype_mainscript)
                    { // We just loaded a main script. Do we want to load the default main script parameters instead?
                        if (_mainScriptIsDefaultMainScript)
                        { // Yes!
                            // For now we just clear all parameters! (in future we might load default  parameters)
                            delete scriptParameters;
                            scriptParameters=new CLuaScriptParameters();
                        }
                    }
                }

                if (theName.compare("Coi")==0)
                {
                    noHit=false;
                    ar >> byteQuantity;
                    ar >> _objectIDAttachedTo_callback;
                }
                if (theName.compare("Coc")==0)
                {
                    noHit=false;
                    ar >> byteQuantity;
                    ar >> _objectIDAttachedTo_customization;
                }
                if (theName.compare("Cod")==0)
                {
                    noHit=false;
                    ar >> byteQuantity; // never use that info, unless loading unknown data!!!! (undo/redo stores dummy info in there)
                    _customObjectData=new CCustomData();
                    _customObjectData->serializeData(ar);
                }

                if (noHit)
                    ar.loadUnknownData();
            }
        }

        handleVerSpec_adjustScriptText1(this,backwardCompatibility_7_8_2014,executeInSensingPhase_oldCompatibility_7_8_2014);
        handleVerSpec_adjustScriptText2(this,(!backwardCompatibility_7_8_2014)&&backwardCompatibility_13_8_2014);
        handleVerSpec_adjustScriptText3(this,backwardCompatibilityCorrectionNeeded_13_10_2014);
        handleVerSpec_adjustScriptText4(this,backwardCompatibilityCorrectionNeeded_8_11_2014);
        handleVerSpec_adjustScriptText5(this,ar.getVrepVersionThatWroteThisFile()<30300);
        handleVerSpec_adjustScriptText6(this,ar.getVrepVersionThatWroteThisFile()<30300);
        handleVerSpec_adjustScriptText7(this,(ar.getVrepVersionThatWroteThisFile()<=30302)&&(VREP_PROGRAM_VERSION_NB>30302));
        handleVerSpec_adjustScriptText8(this,App::userSettings->changeScriptCodeForNewApiNotation);
        handleVerSpec_adjustScriptText9(this);
        handleVerSpec_adjustScriptText10(this,ar.getVrepVersionThatWroteThisFile()<30401);

        fromBufferToFile();
    }
}


int CLuaScriptObject::appendTableEntry(const char* arrayName,const char* keyName,const char* data,const int what[2])
{ // DEPRECATED since 23/2/2016
    if ( (L==NULL)&&_threadedExecution )
        return(-1); // threaded scripts must be called from within!

    if (_threadedExecution)
    {
        if (!VThread::areThreadIDsSame(_threadedScript_associatedFiberOrThreadID,VThread::getCurrentThreadId()))
            return(-1); // only from the same thread when threaded!!
    }

    if (_scriptIsDisabled)
        return(-1);

    if (L==NULL)
    {  // this script was never run before!
        if (_scriptType==sim_scripttype_generalcallback)
            runGeneralCallback_OLD(-1,0,NULL); // dummy call
        if (_scriptType==sim_scripttype_customizationscript)
            runCustomizationScript(sim_syscb_init,NULL,NULL); // dummy call
        if (!App::ct->simulation->isSimulationStopped())
        { // following only when simulation is running!
            if (_scriptType==sim_scripttype_contactcallback)
            {
                int in[3]={0,0,-1};
                int out1[3];
                float out2[14];
                runContactCallback_OLD(in,out1,out2); // dummy call
            }
            if (_scriptType==sim_scripttype_childscript)
                runNonThreadedChildScript(sim_syscb_init,NULL,NULL); // dummy call
            if (_scriptType==sim_scripttype_jointctrlcallback)
            {
                std::vector<bool> inDataBool;
                std::vector<int> inDataInt;
                std::vector<float> inDataFloat;
                std::vector<float> outData;
                std::vector<bool> outDataValidity;
                inDataBool.push_back(true); // init
                inDataBool.push_back(true); // revolute
                inDataBool.push_back(true); // cyclic
                inDataInt.push_back(-1); // joint ID
                inDataInt.push_back(0); // pass count
                inDataInt.push_back(0); // total passes
                inDataFloat.push_back(0.0f); // current pos
                inDataFloat.push_back(0.0f); // target pos
                inDataFloat.push_back(0.0f); // error v
                inDataFloat.push_back(0.0f); // effort
                inDataFloat.push_back(0.0f); // dyn step size
                inDataFloat.push_back(0.0f); // low limit
                inDataFloat.push_back(0.0f); // high limit
                inDataFloat.push_back(0.0f); // target vel
                inDataFloat.push_back(0.0f); // max force
                inDataFloat.push_back(0.0f); // upper vel limit
                runJointCtrlCallback_OLD(inDataBool,inDataInt,inDataFloat,outData,outDataValidity); // dummy call
            }
        }
    }

    if (L==NULL)
        return(-1);

    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    // First check if the table where we want to append a value exists. If not, or not a table, create it!
    luaWrap_lua_getglobal(L,arrayName);
    if (!luaWrap_lua_istable(L,-1))
    { // the table is inexistant
        luaWrap_lua_newtable(L);
        luaWrap_lua_setglobal(L,arrayName);
    }
    luaWrap_lua_pop(L,1);

    // The table where we want to append a value:
    luaWrap_lua_getglobal(L,arrayName);
    int theTablePos=luaWrap_lua_gettop(L);
    int theTableLength=int(luaWrap_lua_objlen(L,theTablePos));

    // Do we want to simply insert the value, or do we want to insert a keyed value?
    if ( (keyName==NULL)||(strlen(keyName)==0) )
    { // not keyed value:
    }
    else
    { // keyed value:
        luaWrap_lua_pushstring(L,keyName);
    }

    // Now push the value, which might itself be a table:
    int w=what[0];
    if ( (w&sim_script_arg_table)!=0 )
    { // we have a table
        w-=sim_script_arg_table;
        luaWrap_lua_newtable(L);
        int newTablePos=luaWrap_lua_gettop(L);
        int stringOff=0;
        for (int i=0;i<what[1];i++)
        {
            if (w==sim_script_arg_null)
                luaWrap_lua_pushnil(L);
            if (w==sim_script_arg_bool)
                luaWrap_lua_pushboolean(L,data[i]);
            if (w==sim_script_arg_int32)
                luaWrap_lua_pushinteger(L,((int*)data)[i]);
            if (w==sim_script_arg_float)
                luaWrap_lua_pushnumber(L,((float*)data)[i]);
            if (w==sim_script_arg_double)
                luaWrap_lua_pushnumber(L,((double*)data)[i]);
            if (w==sim_script_arg_string)
            {
                luaWrap_lua_pushstring(L,data+stringOff);
                stringOff+=int(strlen(data+stringOff))+1;
            }
            luaWrap_lua_rawseti(L,newTablePos,i+1);
        }
    }
    else
    { // we don't have a table
        if (w==sim_script_arg_null)
            luaWrap_lua_pushnil(L);
        if (w==sim_script_arg_bool)
            luaWrap_lua_pushboolean(L,data[0]);
        if (w==sim_script_arg_int32)
            luaWrap_lua_pushinteger(L,((int*)data)[0]);
        if (w==sim_script_arg_float)
            luaWrap_lua_pushnumber(L,((float*)data)[0]);
        if (w==sim_script_arg_double)
            luaWrap_lua_pushnumber(L,((double*)data)[0]);
        if (w==sim_script_arg_string)
            luaWrap_lua_pushstring(L,data);
        if (w==sim_script_arg_charbuff)
            luaWrap_lua_pushlstring(L,data,what[1]);
    }

    // Finally, insert the value in the table:
    if ( (keyName==NULL)||(strlen(keyName)==0) )
    { // not keyed value:
        luaWrap_lua_rawseti(L,theTablePos,theTableLength+1);
    }
    else
    { // keyed value:
        luaWrap_lua_settable(L,-3);
    }

    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    return(0);
}

int CLuaScriptObject::_luaPCall(luaWrap_lua_State* luaState,int nargs,int nresult,int errfunc)
{
    _inExecutionNow=true;
    int retVal=luaWrap_lua_pcall(luaState,nargs,nresult,errfunc);
    _inExecutionNow=false;
    return(retVal);
}

bool CLuaScriptObject::_luaLoadBuffer(luaWrap_lua_State* luaState,const char* buff,size_t sz,const char* name)
{
    // This is the slow version (loading and compiling the buffer over and over):
    // int loadBufferRes=luaWrap_luaL_loadbuffer(luaState,buff,sz,name);
    // return(loadBufferRes==0);

    // This is the faster version (loading and compiling the buffer once only):
    if (_loadBufferResult!=0)
    {
        _loadBufferResult=luaWrap_luaL_loadbuffer(luaState,buff,sz,name);
        if (_loadBufferResult==0)
            luaWrap_lua_setglobal(L,"sim_code_function_to_run");
    }
    if (_loadBufferResult==0)
        luaWrap_lua_getglobal(L,"sim_code_function_to_run");
    return(_loadBufferResult==0);
}


void CLuaScriptObject::setAutomaticCascadingCallsDisabled_OLD(bool disabled)
{
    _automaticCascadingCallsDisabled_OLD=disabled;
}

bool CLuaScriptObject::getAutomaticCascadingCallsDisabled_OLD() const
{
    return(_automaticCascadingCallsDisabled_OLD);
}

int CLuaScriptObject::getObjectIDThatScriptIsAttachedTo_callback_OLD() const
{
    return(_objectIDAttachedTo_callback);
}

void CLuaScriptObject::setObjectIDThatScriptIsAttachedTo_callback_OLD(int newObjectID)
{
    _objectIDAttachedTo_callback=newObjectID;
}

void CLuaScriptObject::runJointCtrlCallback_OLD(const std::vector<bool>& inDataBool,const std::vector<int>& inDataInt,const std::vector<float>& inDataFloat,std::vector<float>& outDataFloat,std::vector<bool>& outDataValidity)
{
    if (inDataBool[0]&&hasLuaState()&&(!_compatibilityModeOrFirstTimeCall_sysCallbacks) )
        killLuaState();
    _runJointCtrlCallback_OLD(sim_syscb_jointcallback,inDataBool,inDataInt,inDataFloat,outDataFloat,outDataValidity);
}

void CLuaScriptObject::_runJointCtrlCallback_OLD(int callType,const std::vector<bool>& inDataBool,const std::vector<int>& inDataInt,const std::vector<float>& inDataFloat,std::vector<float>& outDataFloat,std::vector<bool>& outDataValidity)
{
    FUNCTION_DEBUG;
    if (_scriptTextExec.size()==0)
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());

    _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)

    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState(getScriptSuffixNumberString().c_str());
        callType=sim_syscb_init;
    }

    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    if (_compatibilityModeOrFirstTimeCall_sysCallbacks)
    {
        if (callType!=sim_syscb_cleanup)
        {
            std::string tmp("sim_current_script_id=");
            tmp+=boost::lexical_cast<std::string>(getScriptID());
            luaWrap_luaL_dostring(L,tmp.c_str());
            if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
            {
                for (size_t i=0;i<inDataBool.size();i++)
                    luaWrap_lua_pushboolean(L,inDataBool[i]);
                for (size_t i=0;i<inDataInt.size();i++)
                    luaWrap_lua_pushnumber(L,inDataInt[i]);
                for (size_t i=0;i<inDataFloat.size();i++)
                    luaWrap_lua_pushnumber(L,inDataFloat[i]);
                luaWrap_lua_getglobal(L,"debug");
                luaWrap_lua_getfield(L,-1,"traceback");
                luaWrap_lua_remove(L,-2);
                int argCnt=int(inDataBool.size())+int(inDataInt.size())+int(inDataFloat.size());
                int errIndex=-argCnt-2;
                luaWrap_lua_insert(L,errIndex);
                if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errIndex)!=0)
                { // a runtime error occurred!
                    std::string errMsg;
                    if (luaWrap_lua_isstring(L,-1))
                        errMsg=std::string(luaWrap_lua_tostring(L,-1));
                    else
                        errMsg="(error unknown)";
                    luaWrap_lua_pop(L,-1); // pop error from stack
                    _displayScriptError(errMsg.c_str(),1);
                    // Here we can decide what happens uppon error:
                    App::ct->simulation->pauseOnErrorRequested();
                }
                else
                { // return values:
                    int numberOfArgs=luaWrap_lua_gettop(L); // the first arg is linked to the debug mechanism
                    for (int i=1;i<numberOfArgs;i++)
                    {
                        if (luaWrap_lua_isnumber(L,i+1))
                        {
                            outDataFloat.push_back(float(luaWrap_lua_tonumber(L,i+1)));
                            outDataValidity.push_back(true);
                        }
                    }
                    while (outDataValidity.size()<2)
                    {
                        outDataFloat.push_back(0.0);
                        outDataValidity.push_back(false);
                    }
                    if (callType==sim_syscb_init)
                    {
                        std::string initCb=getSystemCallbackString(sim_syscb_init,false);
                        luaWrap_lua_getglobal(L,initCb.c_str());
                        _compatibilityModeOrFirstTimeCall_sysCallbacks=!(luaWrap_lua_isfunction(L,-1));
                        luaWrap_lua_pop(L,-1);
                    }
                }
                _numberOfPasses++;
            }
            else
            { // A compilation error occurred!
                std::string errMsg;
                if (luaWrap_lua_isstring(L,-1))
                    errMsg=std::string(luaWrap_lua_tostring(L,-1));
                else
                    errMsg="(error unknown)";
                _displayScriptError(errMsg.c_str(),0);
                luaWrap_lua_pop(L,-1); // pop error from stack
                // Here we can decide what happens uppon error:
                App::ct->simulation->pauseOnErrorRequested();
            }
        }
    }

    if (!_compatibilityModeOrFirstTimeCall_sysCallbacks)
    {
        while (true)
        {
            outDataFloat.clear();
            outDataValidity.clear();
            // Push the function name onto the stack (will be automatically popped from stack after _luaPCall):
            luaWrap_lua_getglobal(L,getSystemCallbackString(callType,false).c_str());
            if (luaWrap_lua_isfunction(L,-1))
            { // ok, the function exists!
                CInterfaceStack stack;
                stack.pushTableOntoStack();
                if (callType==sim_syscb_init)
                {
                    stack.pushStringOntoStack("revolute",0);
                    stack.pushBoolOntoStack(inDataBool[1]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("cyclic",0);
                    stack.pushBoolOntoStack(inDataBool[2]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("handle",0);
                    stack.pushNumberOntoStack(inDataInt[0]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("lowLimit",0);
                    stack.pushNumberOntoStack(inDataFloat[5]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("highLimit",0);
                    stack.pushNumberOntoStack(inDataFloat[6]);
                    stack.insertDataIntoStackTable();
                }
                if (callType==sim_syscb_jointcallback)
                {
                    stack.pushStringOntoStack("passCnt",0);
                    stack.pushNumberOntoStack(inDataInt[1]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("totalPasses",0);
                    stack.pushNumberOntoStack(inDataInt[2]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("currentPos",0);
                    stack.pushNumberOntoStack(inDataFloat[0]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("targetPos",0);
                    stack.pushNumberOntoStack(inDataFloat[1]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("errorValue",0);
                    stack.pushNumberOntoStack(inDataFloat[2]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("effort",0);
                    stack.pushNumberOntoStack(inDataFloat[3]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("dynStepSize",0);
                    stack.pushNumberOntoStack(inDataFloat[4]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("targetVel",0);
                    stack.pushNumberOntoStack(inDataFloat[7]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("maxForce",0);
                    stack.pushNumberOntoStack(inDataFloat[8]);
                    stack.insertDataIntoStackTable();
                    stack.pushStringOntoStack("velUpperLimit",0);
                    stack.pushNumberOntoStack(inDataFloat[9]);
                    stack.insertDataIntoStackTable();
                }
                if (callType!=sim_syscb_cleanup)
                    stack.buildOntoLuaStack(L,false);
                luaWrap_lua_getglobal(L,"debug");
                luaWrap_lua_getfield(L,-1,"traceback");
                luaWrap_lua_remove(L,-2);
                int argCnt=stack.getStackSize();
                if (callType==sim_syscb_cleanup)
                    argCnt=0;
                int errIndex=-argCnt-2;
                luaWrap_lua_insert(L,errIndex);
                if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errIndex)!=0)
                { // a runtime error occurred!
                    std::string errMsg;
                    if (luaWrap_lua_isstring(L,-1))
                        errMsg=std::string(luaWrap_lua_tostring(L,-1));
                    else
                        errMsg="(error unknown)";
                    luaWrap_lua_pop(L,-1); // pop error from stack
                    _displayScriptError(errMsg.c_str(),1);
                    // Here we can decide what happens uppon error:
                    App::ct->simulation->pauseOnErrorRequested();
                }
                else
                { // return values:
                    int numberOfArgs=luaWrap_lua_gettop(L)-1; // the first arg is linked to the debug mechanism
                    float forceTorque=0;
                    float velocity=0;
                    bool forceSet=false;
                    bool velocitySet=false;
                    if (numberOfArgs>=1)
                    {
                        stack.clear();
                        stack.buildFromLuaStack(L,1+1,1); // the first arg is linked to the debug mechanism
                        forceSet=stack.getStackMapFloatValue("force",forceTorque);
                        velocitySet=stack.getStackMapFloatValue("velocity",velocity);
                    }
                    outDataFloat.push_back(forceTorque);
                    outDataValidity.push_back(forceSet);
                    outDataFloat.push_back(velocity);
                    outDataValidity.push_back(velocitySet);
                }

            }
            else
                luaWrap_lua_pop(L,-1); // pop the function name
            if (callType!=sim_syscb_init)
                break;
            else
                callType=sim_syscb_jointcallback;
        }
    }
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    _scriptExecStartTime=-1; // means: not initialized yet
}

int CLuaScriptObject::runContactCallback_OLD(const int inDataInt[3],int outDataInt[3],float outDataFloat[14])
{ // args are not very generic, but for now that works fine
    int retVal=0;
    if (_scriptTextExec.size()==0)
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());

    _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)

    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState("");
    }

    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    luaWrap_luaL_dostring(L,tmp.c_str());

    if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
    {
        for (int i=0;i<3;i++)
            luaWrap_lua_pushnumber(L,inDataInt[i]);
        luaWrap_lua_getglobal(L,"debug");
        luaWrap_lua_getfield(L,-1,"traceback");
        luaWrap_lua_remove(L,-2);
        int argCnt=3;
        int errIndex=-argCnt-2;
        luaWrap_lua_insert(L,errIndex);
        if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errIndex)!=0)
        { // a runtime error occurred!
            std::string errMsg;
            if (luaWrap_lua_isstring(L,-1))
                errMsg=std::string(luaWrap_lua_tostring(L,-1));
            else
                errMsg="(error unknown)";
            luaWrap_lua_pop(L,-1); // pop error from stack

            _displayScriptError(errMsg.c_str(),1);
        }
        else
        { // return values:
            int numberOfArgs=luaWrap_lua_gettop(L); // the first arg is linked to the debug mechanism
            if (numberOfArgs>=2)
            {
                retVal=luaToInt(L,2);
                if (retVal>0)
                {
                    if ((numberOfArgs>=4)&&luaWrap_lua_istable(L,3)&&luaWrap_lua_istable(L,4))
                    { // we expect 2 tables!
                        getIntsFromTable(L,3,SIM_MIN(int(luaWrap_lua_objlen(L,3)),3),outDataInt);
                        getFloatsFromTable(L,4,SIM_MIN(int(luaWrap_lua_objlen(L,4)),14),outDataFloat);
                    }
                    else
                        retVal=0; // error
                }
            }
        }
    }
    else
    { // A compilation error occurred!
        std::string errMsg;
        if (luaWrap_lua_isstring(L,-1))
            errMsg=std::string(luaWrap_lua_tostring(L,-1));
        else
            errMsg="(error unknown)";
        _displayScriptError(errMsg.c_str(),0);
        luaWrap_lua_pop(L,-1); // pop error from stack
    }
    _numberOfPasses++;
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    _scriptExecStartTime=-1; // means: not initialized yet
    return(retVal);
}

int CLuaScriptObject::runGeneralCallback_OLD(int callbackId,int callbackTag,void* additionalData)
{
    int retVal=-1; // means error
    if (_scriptTextExec.size()==0)
        _scriptTextExec.assign(_scriptText.begin(),_scriptText.end());

    _scriptExecStartTime=VDateTime::getTimeInMs();
    CApiErrors::pushLocation(scriptID); // for correct error handling (i.e. assignement to the correct script and output)

    if (L==NULL)
    {
        _errorReportMode=sim_api_error_output|sim_api_warning_output;
        _lastErrorString=SIM_API_CALL_NO_ERROR;
        L=initializeNewLuaState("");
    }

    int oldTop=luaWrap_lua_gettop(L);   // We store lua's stack

    std::string tmp("sim_current_script_id=");
    tmp+=boost::lexical_cast<std::string>(getScriptID());
    tmp+="\nsim_callback_id=";
    tmp+=boost::lexical_cast<std::string>(callbackId);
    tmp+="\nsim_callback_tag=";
    tmp+=boost::lexical_cast<std::string>(callbackTag);
    luaWrap_luaL_dostring(L,tmp.c_str());

    if (_luaLoadBuffer(L,_scriptTextExec.c_str(),_scriptTextExec.size(),getShortDescriptiveName().c_str()))
    {
        luaWrap_lua_getglobal(L,"debug");
        luaWrap_lua_getfield(L,-1,"traceback");
        luaWrap_lua_remove(L,-2);
        int argCnt=0;
        int errIndex=-argCnt-2;
        luaWrap_lua_insert(L,errIndex);
        if (_luaPCall(L,argCnt,luaWrapGet_LUA_MULTRET(),errIndex)!=0)
        { // a runtime error occurred!
            std::string errMsg;
            if (luaWrap_lua_isstring(L,-1))
                errMsg=std::string(luaWrap_lua_tostring(L,-1));
            else
                errMsg="(error unknown)";
            luaWrap_lua_pop(L,-1); // pop error from stack

            _displayScriptError(errMsg.c_str(),1);
        }
        else
        { // return values:
            int numberOfArgs=luaWrap_lua_gettop(L); // the first arg is linked to the debug mechanism
            if (numberOfArgs>=2)
                retVal=luaToInt(L,2);
        }
    }
    else
    { // A compilation error occurred!
        std::string errMsg;
        if (luaWrap_lua_isstring(L,-1))
            errMsg=std::string(luaWrap_lua_tostring(L,-1));
        else
            errMsg="(error unknown)";
        _displayScriptError(errMsg.c_str(),0);
        luaWrap_lua_pop(L,-1); // pop error from stack
    }
    _numberOfPasses++;
    luaWrap_lua_settop(L,oldTop);       // We restore lua's stack
    CApiErrors::popLocation(); // for correct error handling (i.e. assignement to the correct script and output)
    _scriptExecStartTime=-1; // means: not initialized yet
    return(retVal);
}

bool CLuaScriptObject::checkAndSetWarningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014()
{
    bool retVal=_warningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014;
    _warningAboutSimHandleChildScriptAlreadyIssued_oldCompatibility_7_8_2014=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_simRMLPosition_oldCompatibility_30_8_2014()
{
    bool retVal=_warning_simRMLPosition_oldCompatibility_30_8_2014;
    _warning_simRMLPosition_oldCompatibility_30_8_2014=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_simRMLVelocity_oldCompatibility_30_8_2014()
{
    bool retVal=_warning_simRMLVelocity_oldCompatibility_30_8_2014;
    _warning_simRMLVelocity_oldCompatibility_30_8_2014=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_simGetMpConfigForTipPose_oldCompatibility_21_1_2016()
{
    bool retVal=_warning_simGetMpConfigForTipPose_oldCompatibility_21_1_2016;
    _warning_simGetMpConfigForTipPose_oldCompatibility_21_1_2016=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_simFindIkPath_oldCompatibility_2_2_2016()
{
    bool retVal=_warning_simFindIkPath_oldCompatibility_2_2_2016;
    _warning_simFindIkPath_oldCompatibility_2_2_2016=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_oldPathPlanningFunctionality_oldCompatibility_11_2_2016()
{
    bool retVal=_warning_oldPathPlanningFunctionality_oldCompatibility_11_2_2016;
    _warning_oldPathPlanningFunctionality_oldCompatibility_11_2_2016=true;
    return(retVal);
}
bool CLuaScriptObject::checkAndSetWarning_oldMotionPlanningFunctionality_oldCompatibility_11_2_2016()
{
    bool retVal=_warning_oldMotionPlanningFunctionality_oldCompatibility_11_2_2016;
    _warning_oldMotionPlanningFunctionality_oldCompatibility_11_2_2016=true;
    return(retVal);
}

