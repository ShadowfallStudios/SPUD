#include "SpudSubsystem.h"
#include "SpudState.h"
#include "Engine/LevelStreaming.h"
#include "Engine/LocalPlayer.h"
#include "Kismet/GameplayStatics.h"
#include "ImageUtils.h"
#include "TimerManager.h"
#include "HAL/FileManager.h"
#include "Async/Async.h"
#include "Streaming/LevelStreamingDelegates.h"

#define USE_UE_SAVE_SYSTEM (PREFER_UE_SAVE_SYSTEM || PLATFORM_PS5)

#if USE_UE_SAVE_SYSTEM
#include "PlatformFeatures.h"
#include "SaveGameSystem.h"
#endif

DEFINE_LOG_CATEGORY(LogSpudSubsystem)

static bool bEnableSPUD = true;
static FAutoConsoleVariableRef CVarEnableSPUD(TEXT("SPUD.Enable"), bEnableSPUD, TEXT("Can be used to debug disable state of plugin by setting to false"), ECVF_Cheat);

void USpudSubsystem::Initialize(FSubsystemCollectionBase& Collection)
{
	bIsTearingDown = false;
	// Note: this will register for clients too, but callbacks will be ignored
	// We can't call ServerCheck() here because GameMode won't be valid (which is what we use to determine server mode)
	FCoreUObjectDelegates::PostLoadMapWithWorld.AddUObject(this, &USpudSubsystem::OnPostLoadMap);
	FCoreUObjectDelegates::PreLoadMap.AddUObject(this, &USpudSubsystem::OnPreLoadMap);

	FLevelStreamingDelegates::OnLevelBeginMakingVisible.AddUObject(this, &USpudSubsystem::OnLevelBeginMakingVisible);
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.AddUObject(this, &USpudSubsystem::OnLevelBeginMakingInvisible);

	FWorldDelegates::OnSeamlessTravelStart.AddUObject(this, &USpudSubsystem::OnSeamlessTravelStart);
	
#if WITH_EDITORONLY_DATA
	// The one problem we have is that in PIE mode, PostLoadMap doesn't get fired for the current map you're on
	// So we'll need to trigger it manually
	// Also "AlwaysLoaded" maps do NOT trigger PostLoad, and at this point, they're NOT in the level list, meaning if
	// we try to sub to levels right now, we'll only see the PersistentLevel
	// So, we're going to have to delay this call by a frame
	auto World = GetWorld();
	if (World && World->WorldType == EWorldType::PIE)
	{
		GetWorld()->GetTimerManager().SetTimerForNextTick([this]()
		{
			// TODO: make this more configurable, use a known save etc
			NewGame(true);
		});
	}
#endif
}

void USpudSubsystem::Deinitialize()
{
	Super::Deinitialize();
	bIsTearingDown = true;
	
	FCoreUObjectDelegates::PostLoadMapWithWorld.RemoveAll(this);
	FCoreUObjectDelegates::PreLoadMap.RemoveAll(this);
	FLevelStreamingDelegates::OnLevelBeginMakingVisible.RemoveAll(this);
	FLevelStreamingDelegates::OnLevelBeginMakingInvisible.RemoveAll(this);
	FWorldDelegates::OnSeamlessTravelStart.RemoveAll(this);
}


void USpudSubsystem::NewGame(bool bCheckServerOnly, bool bAfterLevelLoad)
{
	if (bCheckServerOnly && !ServerCheck(true))
		return;
		
	EndGame();
	
	// EndGame will have unsubscribed from all current levels
	// Re-sub if we want to keep state for currently loaded levels, or not if starting from next level load
	// This allows the caller to call NewGame mid-game and then load a map, and the current levels won't try to save
	if (bAfterLevelLoad)
	{
		CurrentState = ESpudSystemState::NewGameOnNextLevel;
	}
	else
	{
		CurrentState = ESpudSystemState::RunningIdle;
		SubscribeAllLevelObjectEvents();
	}
}

bool USpudSubsystem::ServerCheck(bool LogWarning) const
{
	if (!bEnableSPUD)
	{
		return false;
	}

	// Note: must only call this when game mode is present! Don't call when unloading
	// On missing world etc we just assume true for safety
	auto GI = GetGameInstance();
	if (!GI)
		return true;

	auto World = GI->GetWorld();
	if (!World)
		return true;
	
	return World->GetAuthGameMode() != nullptr;
}

void USpudSubsystem::EndGame()
{
	if (ActiveState)
		ActiveState->ResetState();
	
	// Allow GC to collect
	ActiveState = nullptr;

	UnsubscribeAllLevelObjectEvents();
	CurrentState = ESpudSystemState::Disabled;
	bIsRestoringState = false;
}

void USpudSubsystem::AutoSaveGame(FText Title, bool bTakeScreenshot, const USpudCustomSaveInfo* ExtraInfo)
{
	SaveGame(AutoSaveSlotName,
		Title.IsEmpty() ? NSLOCTEXT("Spud", "AutoSaveTitle", "Autosave") : Title,
		bTakeScreenshot,
		ExtraInfo);
}

void USpudSubsystem::QuickSaveGame(FText Title, bool bTakeScreenshot, const USpudCustomSaveInfo* ExtraInfo)
{
	SaveGame(QuickSaveSlotName,
		Title.IsEmpty() ? NSLOCTEXT("Spud", "QuickSaveTitle", "Quick Save") : Title,
		bTakeScreenshot,
		ExtraInfo);
}

void USpudSubsystem::QuickLoadGame(bool bAutoTravelLevel, const FString& TravelOptions)
{
	LoadGame(QuickSaveSlotName, bAutoTravelLevel, TravelOptions);
}


bool USpudSubsystem::IsQuickSave(const FString& SlotName)
{
	return SlotName == QuickSaveSlotName;
}

bool USpudSubsystem::IsAutoSave(const FString& SlotName)
{
	return SlotName == AutoSaveSlotName;
}

void USpudSubsystem::NotifyLevelLoadedExternally(FName LevelName)
{
	HandleLevelLoaded(LevelName);
}

void USpudSubsystem::NotifyLevelUnloadedExternally(ULevel* Level)
{
	HandleLevelUnloaded(Level);
}

void USpudSubsystem::LoadLatestSaveGame(bool bAutoTravelLevel, const FString& TravelOptions)
{
	auto Latest = GetLatestSaveGame();
	if (Latest)
		LoadGame(Latest->SlotName, bAutoTravelLevel, TravelOptions);
}

void USpudSubsystem::OnPreLoadMap(const FString& MapName)
{
	if (!ServerCheck(false))
	{
		return;
	}

	PreTravelToNewMap.Broadcast(MapName);

	// When we transition out of a map while enabled, save contents
	if (CurrentState == ESpudSystemState::RunningIdle)
	{
		UnsubscribeAllLevelObjectEvents();

		const auto World = GetWorld();
		if (IsValid(World))
		{
			if (bSaveLevelStateWhileTraveling)
			{
				UE_LOG(LogSpudSubsystem, Verbose, TEXT("OnPreLoadMap saving: %s"), *UGameplayStatics::GetCurrentLevelName(World));
				// Map and all streaming level data will be released.
				// Block while doing it so they all get written predictably
				StoreWorld(World, true, true);
			}
			else
			{
				UE_LOG(LogSpudSubsystem, Verbose, TEXT("OnPreLoadMap releasing data: %s"), *UGameplayStatics::GetCurrentLevelName(World));
				for (auto && Level : World->GetLevels())
				{
					GetActiveState()->ReleaseLevelData(USpudState::GetLevelName(Level), true);
				}	
			}
		}
	}
}

void USpudSubsystem::OnSeamlessTravelStart(UWorld* World, const FString& MapName)
{
	if (!ServerCheck(false))
	{
		return;
	}

	if (IsValid(World))
	{
		UE_LOG(LogSpudSubsystem, Verbose, TEXT("OnSeamlessTravelStart: %s"), *MapName);
		// Just before seamless travel, do the same thing as pre load map on OpenLevel
		OnPreLoadMap(MapName);
	}
}

void USpudSubsystem::OnPostLoadMap(UWorld* World)
{
	if (!ServerCheck(false))
	{
		return;
	}

	switch(CurrentState)
	{
	case ESpudSystemState::NewGameOnNextLevel:
		if (IsValid(World)) // nullptr seems possible if load is aborted or something?
		{
			const FString LevelName = UGameplayStatics::GetCurrentLevelName(World);
			UE_LOG(LogSpudSubsystem,
				   Verbose,
				   TEXT("OnPostLoadMap NewGame starting: %s"),
				   *LevelName);
			// We need to subscribe to ALL currently loaded levels, because of "AlwaysLoaded" sublevels
			SubscribeAllLevelObjectEvents();
			CurrentState = ESpudSystemState::RunningIdle;
		}
		break;
	case ESpudSystemState::RunningIdle:
		// We need to subscribe to ALL currently loaded levels, because of "AlwaysLoaded" sublevels
		SubscribeAllLevelObjectEvents();
		break;
	case ESpudSystemState::LoadingGame:
		// This is called when a new map is loaded
		// In all cases, we try to load the state
		if (IsValid(World)) // nullptr seems possible if load is aborted or something?
		{
			const FString LevelName = UGameplayStatics::GetCurrentLevelName(World);
			if (CanRestoreWorld(World))
			{
				UE_LOG(LogSpudSubsystem,
								   Verbose,
								   TEXT("OnPostLoadMap restore: %s"),
								   *LevelName);

				bIsRestoringState = true;

				const auto State = GetActiveState();
				PreLevelRestore.Broadcast(LevelName);
				State->RestoreLoadedWorld(World);
				PostLevelRestore.Broadcast(LevelName, true);

				bIsRestoringState = false;

				// We need to subscribe to ALL currently loaded levels, because of "AlwaysLoaded" sublevels
				SubscribeAllLevelObjectEvents();

				LoadComplete(SlotNameInProgress, true);
				UE_LOG(LogSpudSubsystem, Log, TEXT("Load: Success"));
			}
			else
			{
				UE_LOG(LogSpudState, Log, TEXT("Skipping restore of world %s, no saved data."), *LevelName);

				// We need to subscribe to ALL currently loaded levels, because of "AlwaysLoaded" sublevels
				SubscribeAllLevelObjectEvents();

				LoadComplete(SlotNameInProgress, false);

				UE_LOG(LogSpudSubsystem, Log, TEXT("Load: Skipped"));
			}
		}
		break;
	default:
		break;
	}

	PostTravelToNewMap.Broadcast();
}

void USpudSubsystem::LoadActorData(AActor* Actor, bool bAsGameLoad)
{
	if (!ServerCheck(false))
	{
		return;
	}

	const ESpudSystemState PrevState = CurrentState;

	if (bAsGameLoad)
	{
		CurrentState = ESpudSystemState::LoadingGame;
	}

	UE_LOG(LogSpudSubsystem,
		   Verbose,
		   TEXT("LoadActorData restore: %s"),
		   *GetNameSafe(Actor));

	bIsRestoringState = true;
	const auto State = GetActiveState();
	State->RestoreActor(Actor);
	bIsRestoringState = false;

	CurrentState = PrevState;
}

void USpudSubsystem::MarkActorDestroyed(AActor* Actor)
{
	if (!ServerCheck(false))
	{
		return;
	}
	OnActorDestroyed(Actor);
}

bool USpudSubsystem::IsStreamedLevelRestoring(ULevel* Level) const
{
	const FString LevelName = USpudState::GetLevelName(Level);
	const bool* FoundState = LevelStreamingRestoreStates.Find(FName(LevelName));
	return FoundState && *FoundState;
}

void USpudSubsystem::SaveGame(const FString& SlotName, const FText& Title, bool bTakeScreenshot, const USpudCustomSaveInfo* ExtraInfo)
{
	if (!ServerCheck(true))
	{
		SaveComplete(SlotName, false);
        return;
	}

	if (SlotName.IsEmpty())
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("Cannot save a game with a blank slot name"));		
		SaveComplete(SlotName, false);
		return;
	}

	if (CurrentState != ESpudSystemState::RunningIdle)
	{
		// TODO: ignore or queue?
		UE_LOG(LogSpudSubsystem, Error, TEXT("TODO: Overlapping calls to save/load, resolve this"));
		SaveComplete(SlotName, false);
		return;
	}

	CurrentState = ESpudSystemState::SavingGame;
	PreSaveGame.Broadcast(SlotName);

	if (bTakeScreenshot)
	{
		UE_LOG(LogSpudSubsystem, Verbose, TEXT("Queueing screenshot for save %s"), *SlotName);

		// Memory-based screenshot request
		SlotNameInProgress = SlotName;
		TitleInProgress = Title;
		ExtraInfoInProgress = ExtraInfo;
		UGameViewportClient* ViewportClient = GetGameInstance()->GetGameViewportClient();
		check(ViewportClient);
		OnScreenshotHandle = ViewportClient->OnScreenshotCaptured().AddUObject(this, &USpudSubsystem::OnScreenshotCaptured);
		FScreenshotRequest::RequestScreenshot(false);
	}
	else
	{
		FinishSaveGame(SlotName, Title, ExtraInfo, nullptr);
	}
}

void USpudSubsystem::OnScreenshotCaptured(int32 Width, int32 Height, const TArray<FColor>& Colours)
{
	UGameViewportClient* ViewportClient = UGameplayStatics::GetPlayerController(GetWorld(), 0)->GetLocalPlayer()->ViewportClient;
	ViewportClient->OnScreenshotCaptured().Remove(OnScreenshotHandle);
	OnScreenshotHandle.Reset();

	// Downscale the screenshot, pass to finish
	TArray<FColor> RawDataCroppedResized;
	FImageUtils::CropAndScaleImage(Width, Height, ScreenshotWidth, ScreenshotHeight, Colours, RawDataCroppedResized);

	// Convert down to PNG
	TArray<uint8> PngData;
#if ENGINE_MAJOR_VERSION == 5 && ENGINE_MINOR_VERSION >= 1
	FImageUtils::ThumbnailCompressImageArray(ScreenshotWidth, ScreenshotHeight, RawDataCroppedResized, PngData);
#else
	FImageUtils::CompressImageArray(ScreenshotWidth, ScreenshotHeight, RawDataCroppedResized, PngData);
#endif
	
	FinishSaveGame(SlotNameInProgress, TitleInProgress, ExtraInfoInProgress, &PngData);
	
}
void USpudSubsystem::FinishSaveGame(const FString& SlotName, const FText& Title, const USpudCustomSaveInfo* ExtraInfo, TArray<uint8>* ScreenshotData)
{
	auto State = GetActiveState();
	auto World = GetWorld();

	// We do NOT reset
	// a) deleted objects must remain, they're built up over time
	// b) we may not be updating all levels and must retain for the others

	State->StoreWorldGlobals(World);
	
	for (auto Ptr : GlobalObjects)
	{
		if (Ptr.IsValid())
			State->StoreGlobalObject(Ptr.Get());
	}
	for (auto Pair : NamedGlobalObjects)
	{
		if (Pair.Value.IsValid())
			State->StoreGlobalObject(Pair.Value.Get(), Pair.Key);
	}

	// Store any data that is currently active in the game world in the state object
	StoreWorld(World, false, true);

	State->SetTitle(Title);
	State->SetTimestamp(FDateTime::Now());
	State->SetCustomSaveInfo(ExtraInfo);
	if (ScreenshotData)
		State->SetScreenshot(*ScreenshotData);
	
	// UGameplayStatics::SaveGameToSlot prefixes our save with a lot of crap that we don't need
	// And also wraps it with FObjectAndNameAsStringProxyArchive, which again we don't need
	// Plus it writes it all to memory first, which we don't need another copy of. Write direct to file
	// I'm not sure if the save game system doesn't do this because of some console hardware issues, but
	// I'll worry about that at some later point

	bool SaveOK = false;

#if USE_UE_SAVE_SYSTEM
	if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
	{
		// We need to convert the data to a TArray<uint8> first
		TArray<uint8> Data;
		FMemoryWriter MemoryWriter(Data);
		State->SaveToArchive(MemoryWriter);

		MemoryWriter.Close();

		if (MemoryWriter.IsError() || MemoryWriter.IsCriticalError())
		{
			UE_LOG(LogSpudSubsystem, Error, TEXT("Error while saving game to %s"), *SlotName);
			SaveOK = false;
		}
		else
		{
			// Save to slot
			SaveOK = SaveSystem->SaveGame(false, *SlotName, 0, Data);
			if(SaveOK)
				UE_LOG(LogSpudSubsystem, Log, TEXT("Save to slot %s: Success"), *SlotName);
		}
	}
#else
	IFileManager& FileMgr = IFileManager::Get();
	auto Archive = TUniquePtr<FArchive>(FileMgr.CreateFileWriter(*GetSaveGameFilePath(SlotName)));

	if(Archive)
	{
		State->SaveToArchive(*Archive);
		// Always explicitly close to catch errors from flush/close
		Archive->Close();

		if (Archive->IsError() || Archive->IsCriticalError())
		{
			UE_LOG(LogSpudSubsystem, Error, TEXT("Error while saving game to %s"), *SlotName);
			SaveOK = false;
		}
		else
		{
			UE_LOG(LogSpudSubsystem, Log, TEXT("Save to slot %s: Success"), *SlotName);
			SaveOK = true;
		}
	}
	else
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("Error while creating save game for slot %s"), *SlotName);
		SaveOK = false;
	}
#endif

	SaveComplete(SlotName, SaveOK);
}

void USpudSubsystem::SaveComplete(const FString& SlotName, bool bSuccess)
{
	CurrentState = ESpudSystemState::RunningIdle;
	PostSaveGame.Broadcast(SlotName, bSuccess);
	// It's possible that the reference to SlotName *is* SlotNameInProgress, so we can't reset it until after
	SlotNameInProgress = "";
	TitleInProgress = FText();
	ExtraInfoInProgress = nullptr;
}

void USpudSubsystem::HandleLevelLoaded(FName LevelName)
{
	// Defer the restore to the game thread, streaming calls happen in loading thread?
	// However, quickly ping the state to force it to pre-load the leveldata
	// that way the loading occurs in this thread, less latency
	GetActiveState()->PreLoadLevelData(LevelName.ToString());

	AsyncTask(ENamedThreads::GameThread, [this, LevelName]()
	{
		PostLoadStreamLevelGameThread(LevelName);
		LevelStreamingRestoreStates.Add(LevelName, false);
		PostLoadStreamingLevel.Broadcast(LevelName);
	});
}

void USpudSubsystem::HandleLevelUnloaded(ULevel* Level)
{
	UnsubscribeLevelObjectEvents(Level);

	if (CurrentState != ESpudSystemState::LoadingGame && !bIsTearingDown)
	{
		// NOTE: even though we're attempting to NOT do this while tearing down, in PIE it will still happen on end play
		// This is because for some reason, in PIE the GameInstance shutdown function is called AFTER the levels are flushed,
		// compared to before in normal game shutdown. See the difference between UEditorEngine::EndPlayMap() and UGameEngine::PreExit()
		// We can't really fix this; we could listen on FEditorDelegates::PrePIEEnded but that would require linking the editor module (bleh) 
		// save the state
		// when loading game we will unload the current level and streaming and don't want to restore the active state from that
		// After storing, the level data is released so doesn't take up memory any more
		StoreLevel(Level, true, false);
	}
}



void USpudSubsystem::StoreWorld(UWorld* World, bool bReleaseLevels, bool bBlocking)
{
	for (auto && Level : World->GetLevels())
	{
		StoreLevel(Level, bReleaseLevels, bBlocking);
	}	
}

void USpudSubsystem::StoreLevel(ULevel* Level, bool bRelease, bool bBlocking)
{
	const FString LevelName = USpudState::GetLevelName(Level);
	PreLevelStore.Broadcast(LevelName);
	GetActiveState()->StoreLevel(Level, bRelease, bBlocking);
	PostLevelStore.Broadcast(LevelName, true);
}

void USpudSubsystem::LoadGame(const FString& SlotName, bool bAutoTravelLevel, const FString& TravelOptions)
{
	if (!ServerCheck(true))
	{
		LoadComplete(SlotName, false);
		return;
	}

	if (CurrentState != ESpudSystemState::RunningIdle)
	{
		// TODO: ignore or queue?
		UE_LOG(LogSpudSubsystem, Error, TEXT("TODO: Overlapping calls to save/load, resolve this"));
		LoadComplete(SlotName, false);

		return;
	}

	CurrentState = ESpudSystemState::LoadingGame;
	bIsRestoringState = true;
	PreLoadGame.Broadcast(SlotName);

	UE_LOG(LogSpudSubsystem, Verbose, TEXT("Loading Game from slot %s"), *SlotName);		

	auto State = GetActiveState();

	State->ResetState();

	// TODO: async load

#if USE_UE_SAVE_SYSTEM
	if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
	{
		TArray<uint8> Data;
		FMemoryReader MemoryReader(Data, false);
		MemoryReader.Seek(0);
		if(SaveSystem->LoadGame(false, *SlotName, 0, Data))
		{
			// Load the data from the memory reader
			State->LoadFromArchive(MemoryReader, false);
			// Close Buffer for cache errors
			MemoryReader.Close();
			if (MemoryReader.IsError() || MemoryReader.IsCriticalError())
			{
				UE_LOG(LogSpudSubsystem, Error, TEXT("Error while loading game from %s"), *SlotName);
				LoadComplete(SlotName, false);
				return;
			}
		}
		else
		{
			Data.Empty();
			MemoryReader.Close();
			UE_LOG(LogSpudSubsystem, Error, TEXT("LoadGame: Load Game Returned false, check for inner errors"));
			LoadComplete(SlotName, false);
			return;
		}
	}
	else
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("LoadGame: Platform save system null, cannot load game"));
		LoadComplete(SlotName, false);
		return;
	}
#else
	IFileManager& FileMgr = IFileManager::Get();

	if(auto Archive = TUniquePtr<FArchive>(FileMgr.CreateFileReader(*GetSaveGameFilePath(SlotName))))
	{
		// Load only global data and page in level data as needed
		State->LoadFromArchive(*Archive, false);
		Archive->Close();

		if (Archive->IsError() || Archive->IsCriticalError())
		{
			UE_LOG(LogSpudSubsystem, Error, TEXT("Error while loading game from %s"), *SlotName);
			LoadComplete(SlotName, false);
			return;
		}
	}
	else
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("Error while opening save game for slot %s"), *SlotName);		
		LoadComplete(SlotName, false);
		return;
	}
#endif

	// Just do the reverse of what we did
	// Global objects first before map, these should be only objects which survive map load
	for (auto Ptr : GlobalObjects)
	{
		if (Ptr.IsValid())
			State->RestoreGlobalObject(Ptr.Get());
	}
	for (auto Pair : NamedGlobalObjects)
	{
		if (Pair.Value.IsValid())
			State->RestoreGlobalObject(Pair.Value.Get(), Pair.Key);
	}

	// This is deferred, final load process will happen in PostLoadMap
	SlotNameInProgress = SlotName;

	if (bAutoTravelLevel)
	{
		UE_LOG(LogSpudSubsystem, Verbose, TEXT("(Re)loading map: %s"), *State->GetPersistentLevel());
		UGameplayStatics::OpenLevel(GetWorld(), FName(State->GetPersistentLevel()), true, TravelOptions);
	}
}


void USpudSubsystem::LoadComplete(const FString& SlotName, bool bSuccess)
{
	CurrentState = ESpudSystemState::RunningIdle;
	bIsRestoringState = false;
	SlotNameInProgress = "";
	PostLoadGame.Broadcast(SlotName, bSuccess);
}

bool USpudSubsystem::DeleteSave(const FString& SlotName)
{
	if (!ServerCheck(true))
		return false;
	
#if USE_UE_SAVE_SYSTEM
	if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
		return SaveSystem->DeleteGame(false, *SlotName, 0);
	else
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("DeleteSave: Platform save system null, cannot delete game"));
		return false;
	}
#else
	IFileManager& FileMgr = IFileManager::Get();
	return FileMgr.Delete(*GetSaveGameFilePath(SlotName), false, true);
#endif
}

void USpudSubsystem::AddPersistentGlobalObject(UObject* Obj)
{
	GlobalObjects.AddUnique(TWeakObjectPtr<UObject>(Obj));	
}

void USpudSubsystem::AddPersistentGlobalObjectWithName(UObject* Obj, const FString& Name)
{
	NamedGlobalObjects.Add(Name, Obj);
}

void USpudSubsystem::RemovePersistentGlobalObject(UObject* Obj)
{
	GlobalObjects.Remove(TWeakObjectPtr<UObject>(Obj));
	
	for (auto It = NamedGlobalObjects.CreateIterator(); It; ++It)
	{
		if (It.Value().Get() == Obj)
			It.RemoveCurrent();
	}
}

void USpudSubsystem::ClearLevelState(const FString& LevelName)
{
	GetActiveState()->ClearLevel(LevelName);
	
}

void USpudSubsystem::PostLoadStreamLevelGameThread(FName LevelName)
{
	PostLoadStreamingLevel.Broadcast(LevelName);
	auto StreamLevel = UGameplayStatics::GetStreamingLevel(GetWorld(), LevelName);

	if (StreamLevel)
	{
		ULevel* Level = StreamLevel->GetLoadedLevel();
		
		if (!Level)
		{
			UE_LOG(LogSpudSubsystem, Log, TEXT("PostLoadStreamLevel called for %s but level is null; probably unloaded again?"), *LevelName.ToString());
			return;
		}

		bIsRestoringState = true;

		PreLevelRestore.Broadcast(LevelName.ToString());
		// It's important to note that this streaming level won't be added to UWorld::Levels yet
		// This is usually where things like the TActorIterator get actors from, ULevel::Actors
		// we have the ULevel here right now, so restore it directly
		GetActiveState()->RestoreLevel(Level);

		// NB: after restoring the level, we could release MOST of the memory for this level
		// However, we don't for 2 reasons:
		// 1. Destroyed actors for this level are logged continuously while running, so that still needs to be active
		// 2. We can assume that we'll need to write data back to save when this level is unloaded. It's actually less
		//    memory thrashing to re-use the same memory we have until unload, since it'll likely be almost identical in structure
		StreamLevel->SetShouldBeVisible(true);
		SubscribeLevelObjectEvents(Level);
		PostLevelRestore.Broadcast(LevelName.ToString(), true);

		bIsRestoringState = false;
	}
}

void USpudSubsystem::ForceReset()
{
	CurrentState = ESpudSystemState::RunningIdle;
	bIsRestoringState = false;
}

void USpudSubsystem::SetUserDataModelVersion(int32 Version)
{
	GCurrentUserDataModelVersion = Version;
}


int32 USpudSubsystem::GetUserDataModelVersion() const
{
	return GCurrentUserDataModelVersion;
}


void USpudSubsystem::PostUnloadStreamLevelGameThread(FName LevelName)
{
	PostUnloadStreamingLevel.Broadcast(LevelName);
}

void USpudSubsystem::SubscribeAllLevelObjectEvents()
{
	const auto World = GetWorld();
	if (IsValid(World))
	{
		for (ULevel* Level : World->GetLevels())
		{
			SubscribeLevelObjectEvents(Level);			
		}
	}
}

void USpudSubsystem::UnsubscribeAllLevelObjectEvents()
{
	const auto World = GetWorld();
	if (IsValid(World))
	{
		for (ULevel* Level : World->GetLevels())
		{
			UnsubscribeLevelObjectEvents(Level);			
		}
	}
}

bool USpudSubsystem::CanRestoreLevel(ULevel* Level)
{
	return GetActiveState()->CanRestoreLevel(Level);
}

bool USpudSubsystem::CanRestoreWorld(UWorld* World)
{
	return GetActiveState()->CanRestoreWorld(World);
}

void USpudSubsystem::OnLevelBeginMakingInvisible(UWorld* World, const ULevelStreaming* StreamingLevel, ULevel* LoadedLevel)
{
	if (!ServerCheck(true) || World->IsNetMode(NM_Client))
	{
		return;
	}

	const FString LevelName = USpudState::GetLevelName(LoadedLevel);
	UE_LOG(LogSpudSubsystem, Verbose, TEXT("Level hidden: %s"), *LevelName);
	PreUnloadStreamingLevel.Broadcast(FName(LevelName));
	HandleLevelUnloaded(LoadedLevel);
	PostUnloadStreamingLevel.Broadcast(FName(LevelName));
}

void USpudSubsystem::OnLevelBeginMakingVisible(UWorld* World, const ULevelStreaming* StreamingLevel, ULevel* LoadedLevel)
{
	if (!ServerCheck(true) || World->IsNetMode(NM_Client))
	{
		return;
	}

	const FString LevelNameStr = USpudState::GetLevelName(LoadedLevel);
	UE_LOG(LogSpudSubsystem, Verbose, TEXT("Level shown: %s"), *LevelNameStr);

	// Early return if we do not have anything to load. So we won't change load state
	if (!CanRestoreLevel(LoadedLevel))
	{
		UE_LOG(LogSpudState, Log, TEXT("Skipping restore of streaming level %s, no saved data."), *LevelNameStr);
		return;
	}

	const FName LevelName = FName(LevelNameStr);
	LevelStreamingRestoreStates.Add(LevelName, true);
	PreLoadStreamingLevel.Broadcast(LevelName);
	HandleLevelLoaded(LevelName);
}

void USpudSubsystem::SubscribeLevelObjectEvents(ULevel* Level)
{
	if (Level)
	{
		for (auto Actor : Level->Actors)
		{
			if (!SpudPropertyUtil::IsPersistentObject(Actor))
				continue;			
			// We don't care about runtime spawned actors, only level actors
			// Runtime actors will just be omitted, level actors need to be logged as destroyed
			if (!SpudPropertyUtil::IsRuntimeActor(Actor))
				Actor->OnDestroyed.AddUniqueDynamic(this, &USpudSubsystem::OnActorDestroyed);			
		}		
	}	
}

void USpudSubsystem::UnsubscribeLevelObjectEvents(ULevel* Level)
{
	if (Level)
	{
		for (auto Actor : Level->Actors)
		{
			if (!SpudPropertyUtil::IsPersistentObject(Actor))
				continue;

			if (!SpudPropertyUtil::IsRuntimeActor(Actor))
				Actor->OnDestroyed.RemoveDynamic(this, &USpudSubsystem::OnActorDestroyed);			
		}		
	}	
}

void USpudSubsystem::OnActorDestroyed(AActor* Actor)
{
	if (CurrentState == ESpudSystemState::RunningIdle)
	{
		auto Level = Actor->GetLevel();
		// Ignore actor destruction caused by levels being unloaded
		if (Level && !Level->bIsBeingRemoved)
		{
			auto State = GetActiveState();
			State->StoreLevelActorDestroyed(Actor);
		}
	}
}

struct FSaveSorter
{
	ESpudSaveSorting Sorting;
	
	FSaveSorter(ESpudSaveSorting S) : Sorting(S) {}
	
	FORCEINLINE bool operator()(const USpudSaveGameInfo& A, const USpudSaveGameInfo& B ) const
	{
		switch (Sorting)
		{
		default:
		case ESpudSaveSorting::None:
			return false;
		case ESpudSaveSorting::MostRecent:
			// Reverse ordering
			return A.Timestamp > B.Timestamp;
		case ESpudSaveSorting::SlotName:
			return A.SlotName.Compare(B.SlotName, ESearchCase::IgnoreCase) < 0;
		case ESpudSaveSorting::Title:
			return A.Title.CompareToCaseIgnored(B.Title) < 0;
		}
	}
};

TArray<USpudSaveGameInfo*> USpudSubsystem::GetSaveGameList(bool bIncludeQuickSave, bool bIncludeAutoSave, ESpudSaveSorting Sorting)
{
	TArray<FString> SaveFiles;
	ListSaveGameFiles(SaveFiles);

	TArray<USpudSaveGameInfo*> Ret;
	for (auto&& File : SaveFiles)
	{
#if PLATFORM_PS5
		FString SlotName = File; // Because consoles doesn't have an extension
#else
		FString SlotName = FPaths::GetBaseFilename(File);
#endif

		if ((!bIncludeQuickSave && SlotName == QuickSaveSlotName) ||
			(!bIncludeAutoSave && SlotName == AutoSaveSlotName))
		{
			continue;
		}

		if (auto Info = GetSaveGameInfo(SlotName))
			Ret.Add(Info);
	}

	if (Sorting != ESpudSaveSorting::None)
	{
		Ret.Sort(FSaveSorter(Sorting));
	}

	return Ret;
}

USpudSaveGameInfo* USpudSubsystem::GetSaveGameInfo(const FString& SlotName)
{
#if USE_UE_SAVE_SYSTEM
	if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
	{
		TArray<uint8> Data;
		FMemoryReader MemoryReader(Data, false);
		MemoryReader.Seek(0);
		if (SaveSystem->LoadGame(false, *SlotName, 0, Data))
		{
			auto Info = NewObject<USpudSaveGameInfo>();
			const bool bResult = USpudState::LoadSaveInfoFromArchive(MemoryReader, *Info);
			Info->SlotName = SlotName;

			return bResult ? Info : nullptr;
		}

		//Load Failed
		MemoryReader.FlushCache();
		Data.Empty();
		MemoryReader.Close();
	}
	//Platform Save System is null
	UE_LOG(LogSpudSubsystem, Error, TEXT("GetSaveGameInfo: Platform save system is null, cannot load game"));
	return nullptr;
#else
	IFileManager& FM = IFileManager::Get();
	// We want to parse just the very first part of the file, not all of it
	FString AbsoluteFilename = FPaths::Combine(GetSaveGameDirectory(), SlotName + ".sav");
	auto Archive = TUniquePtr<FArchive>(FM.CreateFileReader(*AbsoluteFilename));

	if(!Archive)
	{
		UE_LOG(LogSpudSubsystem, Error, TEXT("Unable to open %s for reading info"), *AbsoluteFilename);
		return nullptr;
	}

	auto Info = NewObject<USpudSaveGameInfo>();
	Info->SlotName = SlotName;

	const bool bResult = USpudState::LoadSaveInfoFromArchive(*Archive, *Info);
	Archive->Close();

	return bResult ? Info : nullptr;
#endif
}

USpudSaveGameInfo* USpudSubsystem::GetLatestSaveGame()
{
	auto SaveGameList = GetSaveGameList();
	USpudSaveGameInfo* Best = nullptr;
	for (auto Curr : SaveGameList)
	{
		if (!Best || Curr->Timestamp > Best->Timestamp)
			Best = Curr;		
	}
	return Best;
}


USpudSaveGameInfo* USpudSubsystem::GetQuickSaveGame()
{
	return GetSaveGameInfo(QuickSaveSlotName);
}

USpudSaveGameInfo* USpudSubsystem::GetAutoSaveGame()
{
	return GetSaveGameInfo(AutoSaveSlotName);
}

FString USpudSubsystem::GetSaveGameDirectory()
{
	return FString::Printf(TEXT("%sSaveGames/"), *FPaths::ProjectSavedDir());
}

FString USpudSubsystem::GetSaveGameFilePath(const FString& SlotName)
{
	return FString::Printf(TEXT("%s%s.sav"), *GetSaveGameDirectory(), *SlotName);
}

void USpudSubsystem::ListSaveGameFiles(TArray<FString>& OutSaveFileList)
{
#if USE_UE_SAVE_SYSTEM
	if (ISaveGameSystem* SaveSystem = IPlatformFeaturesModule::Get().GetSaveGameSystem())
	{
		SaveSystem->GetSaveGameNames(OutSaveFileList,0);
	}
#else
	IFileManager& FM = IFileManager::Get();

	FM.FindFiles(OutSaveFileList, *GetSaveGameDirectory(), TEXT(".sav"));
#endif
}

FString USpudSubsystem::GetActiveGameFolder()
{
	return FString::Printf(TEXT("%sCurrentGame/"), *FPaths::ProjectSavedDir());
}

FString USpudSubsystem::GetActiveGameFilePath(const FString& Name)
{
	return FString::Printf(TEXT("%sSaveGames/%s.sav"), *GetActiveGameFolder(), *Name);
}


class FUpgradeAllSavesAction : public FPendingLatentAction
{
public:
	FName ExecutionFunction;
	int32 OutputLink;
	FWeakObjectPtr CallbackTarget;

	struct FUpgradeTask : public FNonAbandonableTask
	{
		bool bUpgradeAlways;
		FSpudUpgradeSaveDelegate UpgradeCallback;
		
		FUpgradeTask(bool InUpgradeAlways, FSpudUpgradeSaveDelegate InCallback) : bUpgradeAlways(InUpgradeAlways), UpgradeCallback(InCallback) {}

		bool SaveNeedsUpgrading(const USpudState* State)
		{
			if (State->SaveData.GlobalData.IsUserDataModelOutdated())
				return true;

			for (auto& Pair : State->SaveData.LevelDataMap)
			{
				if (Pair.Value->IsUserDataModelOutdated())
					return true;				
			}

			return false;
		}

		void DoWork()
		{
			if (!UpgradeCallback.IsBound())
				return;
			
			IFileManager& FileMgr = IFileManager::Get();
			TArray<FString> SaveFiles;
			USpudSubsystem::ListSaveGameFiles(SaveFiles);

			for (auto && SaveFile : SaveFiles)
			{
				FString AbsoluteFilename = FPaths::Combine(USpudSubsystem::GetSaveGameDirectory(), SaveFile);
				auto Archive = TUniquePtr<FArchive>(FileMgr.CreateFileReader(*AbsoluteFilename));

				if(Archive)
				{
					auto State = NewObject<USpudState>();
					// Load all data because we want to upgrade
					State->LoadFromArchive(*Archive, true);
					Archive->Close();

					if (Archive->IsError() || Archive->IsCriticalError())
					{
						UE_LOG(LogSpudSubsystem, Error, TEXT("Error while loading game to check for upgrades: %s"), *SaveFile);
						continue;
					}

					if (bUpgradeAlways || SaveNeedsUpgrading(State))
					{
						if (UpgradeCallback.Execute(State))
						{
							// Move aside old save
							FString BackupFilename = AbsoluteFilename + ".bak"; 
							FileMgr.Move(*BackupFilename, *AbsoluteFilename, true, true);
							// Now save
							auto OutArchive = TUniquePtr<FArchive>(FileMgr.CreateFileWriter(*AbsoluteFilename));
							if (OutArchive)
							{
								State->SaveToArchive(*OutArchive);
							}
						}
					}
				}
			}

		}

		FORCEINLINE TStatId GetStatId() const
		{
			RETURN_QUICK_DECLARE_CYCLE_STAT(FUpgradeTask, STATGROUP_ThreadPoolAsyncTasks);
		}
	
	};

	FAsyncTask<FUpgradeTask> UpgradeTask;

	FUpgradeAllSavesAction(bool UpgradeAlways, FSpudUpgradeSaveDelegate InUpgradeCallback, const FLatentActionInfo& LatentInfo)
        : ExecutionFunction(LatentInfo.ExecutionFunction)
        , OutputLink(LatentInfo.Linkage)
        , CallbackTarget(LatentInfo.CallbackTarget)
        , UpgradeTask(UpgradeAlways, InUpgradeCallback)
	{
		// We do the actual upgrade work in a background task, this action is just to monitor when it's done
		UpgradeTask.StartBackgroundTask();
	}

	virtual void UpdateOperation(FLatentResponse& Response) override
	{
		// This is essentially a game thread tick. Finish the latent action when the background task is done
		Response.FinishAndTriggerIf(UpgradeTask.IsDone(), ExecutionFunction, OutputLink, CallbackTarget);
	}

#if WITH_EDITOR
	virtual FString GetDescription() const override
	{
		return "Upgrade All Saves";
	}
#endif
};


void USpudSubsystem::UpgradeAllSaveGames(bool bUpgradeEvenIfNoUserDataModelVersionDifferences,
                                         FSpudUpgradeSaveDelegate SaveNeedsUpgradingCallback,
                                         FLatentActionInfo LatentInfo)
{
	
	FLatentActionManager& LatentActionManager = GetGameInstance()->GetLatentActionManager();
	if (LatentActionManager.FindExistingAction<FUpgradeAllSavesAction>(LatentInfo.CallbackTarget, LatentInfo.UUID) == nullptr)
	{
		LatentActionManager.AddNewAction(LatentInfo.CallbackTarget, LatentInfo.UUID,
		                                 new FUpgradeAllSavesAction(bUpgradeEvenIfNoUserDataModelVersionDifferences,
		                                                            SaveNeedsUpgradingCallback, LatentInfo));
	}
}

USpudCustomSaveInfo* USpudSubsystem::CreateCustomSaveInfo()
{
	return NewObject<USpudCustomSaveInfo>();
}
