#include <Core/CorePCH.h>

#include <Core/ActorSystem/ActorApiService.h>

// clang-format off
EZ_BEGIN_DYNAMIC_REFLECTED_TYPE(ezActorApiService, 1, ezRTTINoAllocator)
EZ_END_DYNAMIC_REFLECTED_TYPE;
// clang-format on

ezActorApiService::ezActorApiService() = default;
ezActorApiService::~ezActorApiService() = default;



EZ_STATICLINK_FILE(Core, Core_ActorSystem_Implementation_ActorApiService);
