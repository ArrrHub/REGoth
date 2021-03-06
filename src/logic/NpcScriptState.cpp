#include "NpcScriptState.h"
#include <components/Vob.h>
#include <ZenLib/utils/logger.h>
#include <components/VobClasses.h>
#include "PlayerController.h"

using namespace Logic;

/**
 * Valid script-states an NPC can be in
 */
static const std::string s_EnabledPlayerStates[] = {
        "ZS_ASSESSMAGIC",
        "ZS_ASSESSSTOPMAGIC",
        "ZS_MAGICFREEZE",
        "ZS_WHIRLWIND",
        "ZS_SHORTZAPPED",
        "ZS_ZAPPED",
        "ZS_PYRO",
        "ZS_MAGICSLEEP"
};

NpcScriptState::NpcScriptState(World::WorldInstance& world, Handle::EntityHandle hostVob) :
    m_World(world),
	m_HostVob(hostVob)
{

}

NpcScriptState::~NpcScriptState()
{

}


bool Logic::NpcScriptState::startAIState(size_t symIdx, bool endOldState, bool isRoutineState)
{
    VobTypes::NpcVobInformation vob = VobTypes::asNpcVob(m_World, m_HostVob);
    ScriptEngine& s = m_World.getScriptEngine();
    Daedalus::DATFile& dat = s.getVM().getDATFile();

    m_NextState.name = dat.getSymbolByIndex(symIdx).name;

    if(!dat.hasSymbolName(m_NextState.name + "_END")
        || !dat.hasSymbolName(m_NextState.name + "_LOOP"))
    {
        LogWarn() << "Did not find script functions '" << m_NextState.name + "_END" << "' or '" << m_NextState.name + "_LOOP" << "' in .DAT-file!";
        return false;
    }

    // Check if this is just a usual action (ZS = German "Zustand" = State, B = German "Befehl" = Instruction)
    if(m_NextState.name.substr(0, 3) != "ZS_")
    {
        // Disable routine-flag for this, since scripts could let the npc assess something
        bool oldIsRoutineState = m_CurrentState.isRoutineState;
        m_CurrentState.isRoutineState = isRoutineState;

        // Just call the function
        s.prepareRunFunction();
        s.setInstance("SELF", VobTypes::getScriptObject(vob).instanceSymbol);
        s.runFunctionBySymIndex(symIdx);

        m_CurrentState.isRoutineState = oldIsRoutineState;
        return true;
    }

    m_NextState.isRoutineState = isRoutineState;
    m_NextState.symEnd = dat.getSymbolIndexByName(m_NextState.name + "_END");
    m_NextState.symLoop = dat.getSymbolIndexByName(m_NextState.name + "_LOOP");
    m_NextState.valid = true;

    if(endOldState)
    {
        m_CurrentState.phase = NpcAIState::EPhase::End;
    } else
    {
        // Just interrupt the state
        m_CurrentState.phase = NpcAIState::EPhase::Interrupt;

        // If this is not a player, or the player is valid to perform this state...
        if(!vob.playerController->isPlayerControlled() || canPlayerUseAIState(m_NextState))
        {
            vob.playerController->getModelVisual()->stopAnimations();
            vob.playerController->interrupt();
            vob.playerController->getEM().clear();
        }
    }

	return doAIState(0.0);
}

bool Logic::NpcScriptState::startAIState(const std::string & name, bool endOldState, bool isRoutineState)
{
    // Get script symbol index
    size_t idx = m_World.getScriptEngine().getVM().getDATFile().getSymbolIndexByName(name);

	return startAIState(idx, endOldState, isRoutineState);
}

bool NpcScriptState::canPlayerUseAIState(const NpcAIState& state)
{
    if(state.valid)
    {
        if(state.prgState != EPrgStates::NPC_PRGAISTATE_INVALID)
        {
            // States that are allowed for every NPC at any time
            return (state.prgState == EPrgStates::NPC_PRGAISTATE_DEAD
                    || state.prgState == EPrgStates::NPC_PRGAISTATE_UNCONSCIOUS);
        } else
        {
            for(const std::string& s : s_EnabledPlayerStates)
            {
                if(state.name == s)
                    return true;
            }
        }
    }

    return false;
}

bool NpcScriptState::doAIState(float deltaTime)
{
    VobTypes::NpcVobInformation vob = VobTypes::asNpcVob(m_World, m_HostVob);
    ScriptEngine& s = m_World.getScriptEngine();
    Daedalus::DATFile& dat = s.getVM().getDATFile();

    // Increase time this state is already running
    if(m_CurrentState.valid && m_CurrentState.phase == NpcAIState::EPhase::Loop)
        m_CurrentState.stateTime += deltaTime;

    // Only do states if we do not have messages pending
    if(vob.playerController->getEM().isEmpty())
    {
        // TODO: Routine

        if(!m_CurrentState.valid)
        {
            // Can we move to the next state?
            if(m_NextState.valid)
            {
                // Remember the last state we were in
                m_LastStateSymIndex = m_CurrentState.symIndex;

                // Move to next state
                m_CurrentState = m_NextState;
                m_CurrentState.stateTime = 0.0f;

                m_NextState.valid = false;
            } else
            {
                // TODO: Start routine state here
            }
        }

        // If this is the player, only allow states the player is allowed to have
        if(vob.playerController->isPlayerControlled() && !canPlayerUseAIState(m_CurrentState))
            return false;

        if(m_CurrentState.valid)
        {
            // Prepare state function call
            s.setInstance("SELF", VobTypes::getScriptObject(vob).instanceSymbol);

            // These are set by the game, but seem to be always 0
            //s.setInstance("OTHER", );
            //s.setInstance("VICTIM", );
            //s.setInstance("ITEM", );

            if(m_CurrentState.phase == NpcAIState::EPhase::Uninitialized)
            {
                // TODO: Set perception-time to 5000

                if(m_CurrentState.symIndex > 0)
                {
                    // Call setup-function
                    s.prepareRunFunction();
                    s.runFunctionBySymIndex(m_CurrentState.symIndex);
                }

                // Now do the looping function every frame
                m_CurrentState.phase = NpcAIState::EPhase::Loop;
            }else if(m_CurrentState.phase == NpcAIState::EPhase::Loop)
            {
                bool end = true;

                // Call looping-function
                if(m_CurrentState.symLoop > 0)
                {
                    s.prepareRunFunction();
                    end = s.runFunctionBySymIndex(m_CurrentState.symLoop) != 0;
                }

                // Run a program based state
                if(m_CurrentState.prgState != EPrgStates::NPC_PRGAISTATE_INVALID)
                {
                    // Only CheckUnconscious() is called here. There is also a state for following, but it doesn't do anything here
                    // TODO: call CheckUnconscious
                }

                // Check if we're done and remove the state in the next frame
                if(end)
                    m_CurrentState.phase = NpcAIState::EPhase::End;

            }else if(m_CurrentState.phase == NpcAIState::EPhase::End)
            {
                // Call end-function
                if(m_CurrentState.symEnd > 0)
                {
                    s.prepareRunFunction();
                    s.runFunctionBySymIndex(m_CurrentState.symEnd);
                }

                // Invalidate the state and get it ready for the next one
                m_CurrentState.phase = NpcAIState::EPhase::Interrupt;
                m_CurrentState.valid = false;
            }
        }
    }

    return false;
}

bool NpcScriptState::startRoutineState()
{
    VobTypes::NpcVobInformation npc = VobTypes::asNpcVob(m_World, m_HostVob);

    // Just say "yes" for the player, as he never has any routine
    if(npc.playerController->isPlayerControlled())
        return true;

    // TODO: Implement
    /**
    bool r =

    return r;*/
    return true;
}

