/** @file
  GoogleTests for the VariableSmmRuntimeDxe hook consumer.

  Copyright (c) Microsoft Corporation.
  SPDX-License-Identifier: BSD-2-Clause-Patent

**/

#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>

#include <Library/GoogleTestLib.h>
#include <GoogleTest/Library/MockUefiBootServicesTableLib.h>
#include <GoogleTest/Library/MockUefiRuntimeLib.h>

extern "C" {
  #include <Uefi.h>
  #include <Library/BaseLib.h>
  #include <Library/BaseMemoryLib.h>
  #include <Protocol/VariableSmmRuntimeDxeHook.h>

  #include "../VariableSmmRuntimeDxeHookInternal.h"
}

using namespace testing;

MATCHER_P (GuidPtrEq, Expected, "GUID contents match") {
  return (arg != NULL) && CompareGuid (arg, &Expected);
}

MATCHER_P (UnicodeStrEq, Expected, "Unicode string contents match") {
  return (arg != NULL) && (StrCmp (arg, Expected) == 0);
}

MATCHER_P (ByteDataEq, Expected, "data byte contents match") {
  return (arg != NULL) && (*static_cast<CONST UINT8 *>(arg) == Expected);
}

class MockVariableRuntimeHookProvider {
public:
  MOCK_METHOD (
    EFI_STATUS,
    PreSetVariable,
    (CONST CHAR16 *, CONST EFI_GUID *, UINT32, UINTN, CONST VOID *)
    );
  MOCK_METHOD (VOID, PostSetVariable, (EFI_STATUS));
};

STATIC MockVariableRuntimeHookProvider  *mProviderMock;

STATIC
EFI_STATUS
EFIAPI
TestPreSetVariable (
  IN CONST CHAR16    *VariableName,
  IN CONST EFI_GUID  *VendorGuid,
  IN       UINT32    Attributes,
  IN       UINTN     DataSize,
  IN CONST VOID      *Data
  )
{
  return mProviderMock->PreSetVariable (
                          VariableName,
                          VendorGuid,
                          Attributes,
                          DataSize,
                          Data
                          );
}

STATIC
VOID
EFIAPI
TestPostSetVariable (
  IN EFI_STATUS  SetVariableStatus
  )
{
  mProviderMock->PostSetVariable (SetVariableStatus);
}

class VariableSmmRuntimeDxeHookTest : public Test {
protected:
  StrictMock<MockUefiBootServicesTableLib>  BootServicesMock;
  StrictMock<MockUefiRuntimeLib>            RuntimeLibMock;
  StrictMock<MockVariableRuntimeHookProvider> ProviderMock;

  EDKII_VARIABLE_RUNTIME_HOOK_PROTOCOL  ProviderProtocol;
  std::mutex                            ProviderMutex;
  std::condition_variable               ProviderCondition;
  UINTN                                 ProviderEntryCount;
  BOOLEAN                               SecondProviderEntered;
  BOOLEAN                               SecondCallCompleted;
  BOOLEAN                               ReleaseProvider;

  VOID
  SetUp (
    VOID
    ) override
  {
    ProviderProtocol.PreSetVariable  = TestPreSetVariable;
    ProviderProtocol.PostSetVariable = TestPostSetVariable;
    ProviderEntryCount               = 0;
    SecondProviderEntered            = FALSE;
    SecondCallCompleted              = FALSE;
    ReleaseProvider                  = FALSE;
    mProviderMock                    = &ProviderMock;
  }

  VOID
  TearDown (
    VOID
    ) override
  {
    mProviderMock = NULL;
  }
};

// Verify provider caching, exact-success handling, and that concurrent callers
// each cross the provider's synchronization point before entering MM.
TEST_F (VariableSmmRuntimeDxeHookTest, CachedProviderExactStatusAndConcurrentCalls) {
  STATIC CONST EFI_GUID  VendorGuid = {
    0x0e82b7cb, 0x6f9f, 0x4ab8, { 0x92, 0x4c, 0x21, 0x2d, 0xe8, 0x7b, 0x72, 0xd4 }
  };
  STATIC CONST CHAR16  SuccessName[] = {
    'H', 'o', 'o', 'k', 'S', 'u', 'c', 'c', 'e', 's', 's', '\0'
  };
  STATIC CONST CHAR16  ErrorName[] = {
    'H', 'o', 'o', 'k', 'E', 'r', 'r', 'o', 'r', '\0'
  };
  STATIC CONST CHAR16  WarningName[] = {
    'H', 'o', 'o', 'k', 'W', 'a', 'r', 'n', 'i', 'n', 'g', '\0'
  };
  STATIC CONST CHAR16  ConcurrentName1[] = {
    'H', 'o', 'o', 'k', 'C', 'o', 'n', 'c', 'u', 'r', 'r', 'e', 'n', 't', '1', '\0'
  };
  STATIC CONST CHAR16  ConcurrentName2[] = {
    'H', 'o', 'o', 'k', 'C', 'o', 'n', 'c', 'u', 'r', 'r', 'e', 'n', 't', '2', '\0'
  };
  STATIC CONST UINT8   Data       = 0x5A;
  STATIC CONST UINT32  Attributes = EFI_VARIABLE_BOOTSERVICE_ACCESS | EFI_VARIABLE_RUNTIME_ACCESS;

  EFI_STATUS  Status;
  BOOLEAN     HookInvoked;

  EXPECT_CALL (
    BootServicesMock,
    gBS_LocateProtocol (
      GuidPtrEq (gEdkiiVariableSmmRuntimeDxeHookProtocolGuid),
      IsNull (),
      NotNull ()
      )
    )
    .WillOnce (
       Invoke (
         [this] (EFI_GUID *, VOID *, VOID **Interface) {
    *Interface = &ProviderProtocol;
    return EFI_SUCCESS;
  }
         )
       );
  EXPECT_CALL (RuntimeLibMock, EfiAtRuntime ())
    .Times (6)
    .WillRepeatedly (Return (TRUE));

  EXPECT_CALL (
    ProviderMock,
    PreSetVariable (
      UnicodeStrEq (SuccessName),
      GuidPtrEq (VendorGuid),
      Eq (Attributes),
      Eq (sizeof (Data)),
      ByteDataEq (Data)
      )
    )
    .WillOnce (Return (EFI_SUCCESS));
  EXPECT_CALL (
    ProviderMock,
    PreSetVariable (
      UnicodeStrEq (ErrorName),
      GuidPtrEq (VendorGuid),
      Eq (Attributes),
      Eq (sizeof (Data)),
      ByteDataEq (Data)
      )
    )
    .WillOnce (Return (EFI_DEVICE_ERROR));
  EXPECT_CALL (
    ProviderMock,
    PreSetVariable (
      UnicodeStrEq (ConcurrentName1),
      GuidPtrEq (VendorGuid),
      Eq (Attributes),
      Eq (sizeof (Data)),
      ByteDataEq (Data)
      )
    )
    .WillOnce (
       Invoke (
         [this] (CONST CHAR16 *, CONST EFI_GUID *, UINT32, UINTN, CONST VOID *) {
    std::unique_lock<std::mutex>  Lock (ProviderMutex);

    ProviderEntryCount++;
    ProviderCondition.notify_all ();
    ProviderCondition.wait (Lock, [this] { return ReleaseProvider == TRUE; });
    return EFI_SUCCESS;
  }
         )
       );
  EXPECT_CALL (
    ProviderMock,
    PreSetVariable (
      UnicodeStrEq (ConcurrentName2),
      GuidPtrEq (VendorGuid),
      Eq (Attributes),
      Eq (sizeof (Data)),
      ByteDataEq (Data)
      )
    )
    .WillOnce (
       Invoke (
         [this] (CONST CHAR16 *, CONST EFI_GUID *, UINT32, UINTN, CONST VOID *) {
    std::unique_lock<std::mutex>  Lock (ProviderMutex);

    ProviderEntryCount++;
    SecondProviderEntered = TRUE;
    ProviderCondition.notify_all ();
    ProviderCondition.wait (Lock, [this] { return ReleaseProvider == TRUE; });
    return EFI_SUCCESS;
  }
         )
       );
  EXPECT_CALL (
    ProviderMock,
    PreSetVariable (
      UnicodeStrEq (WarningName),
      GuidPtrEq (VendorGuid),
      Eq (Attributes),
      Eq (sizeof (Data)),
      ByteDataEq (Data)
      )
    )
    .WillOnce (Return (EFI_WARN_UNKNOWN_GLYPH));

  EXPECT_CALL (ProviderMock, PostSetVariable (Eq (EFI_SUCCESS)))
    .Times (1);
  EXPECT_CALL (ProviderMock, PostSetVariable (Eq (EFI_DEVICE_ERROR)))
    .Times (1);
  EXPECT_CALL (ProviderMock, PostSetVariable (Eq (EFI_ABORTED)))
    .Times (1);

  HookInvoked = 0xA5;
  Status      = VariableRuntimeHookPreSetVariable (
                  SuccessName,
                  &VendorGuid,
                  Attributes,
                  sizeof (Data),
                  &Data,
                  &HookInvoked
                  );
  EXPECT_EQ (Status, EFI_SUCCESS);
  EXPECT_EQ (HookInvoked, FALSE);
  VariableRuntimeHookPostSetVariable (HookInvoked, EFI_SUCCESS);

  Status = VariableRuntimeHookInitialize ();
  EXPECT_EQ (Status, EFI_SUCCESS);

  HookInvoked = 0xA5;
  Status      = VariableRuntimeHookPreSetVariable (
                  SuccessName,
                  &VendorGuid,
                  Attributes,
                  sizeof (Data),
                  &Data,
                  &HookInvoked
                  );
  EXPECT_EQ (Status, EFI_SUCCESS);
  EXPECT_EQ (HookInvoked, TRUE);
  VariableRuntimeHookPostSetVariable (HookInvoked, EFI_SUCCESS);

  HookInvoked = 0xA5;
  Status      = VariableRuntimeHookPreSetVariable (
                  ErrorName,
                  &VendorGuid,
                  Attributes,
                  sizeof (Data),
                  &Data,
                  &HookInvoked
                  );
  EXPECT_EQ (Status, EFI_DEVICE_ERROR);
  EXPECT_EQ (HookInvoked, FALSE);

  EFI_STATUS  FirstStatus      = EFI_NOT_READY;
  EFI_STATUS  SecondStatus     = EFI_NOT_READY;
  BOOLEAN     FirstHookInvoked = 0xA5;
  BOOLEAN     SecondHookInvoked = 0xA5;

  std::thread  FirstThread (
    [&] {
    FirstStatus = VariableRuntimeHookPreSetVariable (
                    ConcurrentName1,
                    &VendorGuid,
                    Attributes,
                    sizeof (Data),
                    &Data,
                    &FirstHookInvoked
                    );
  }
    );

  BOOLEAN  FirstProviderEntered;
  {
    std::unique_lock<std::mutex>  Lock (ProviderMutex);

    FirstProviderEntered = ProviderCondition.wait_for (
                                               Lock,
                                               std::chrono::seconds (5),
                                               [this] { return ProviderEntryCount >= 1; }
                                               );
    if (FirstProviderEntered == FALSE) {
      ReleaseProvider = TRUE;
    }
  }

  if (FirstProviderEntered == FALSE) {
    ProviderCondition.notify_all ();
    FirstThread.join ();
    ADD_FAILURE () << "The first provider callback did not reach its synchronization point";
    return;
  }

  std::thread  SecondThread (
    [&] {
    SecondStatus = VariableRuntimeHookPreSetVariable (
                     ConcurrentName2,
                     &VendorGuid,
                     Attributes,
                     sizeof (Data),
                     &Data,
                     &SecondHookInvoked
                     );
    {
      std::lock_guard<std::mutex>  Lock (ProviderMutex);

      SecondCallCompleted = TRUE;
    }

    ProviderCondition.notify_all ();
  }
    );

  BOOLEAN  ObservedSecondProgress;
  BOOLEAN  SecondEnteredBeforeRelease;
  BOOLEAN  SecondCompletedWithoutProvider;
  {
    std::unique_lock<std::mutex>  Lock (ProviderMutex);

    ObservedSecondProgress = ProviderCondition.wait_for (
                                                Lock,
                                                std::chrono::seconds (5),
                                                [this] {
      return (SecondProviderEntered == TRUE) || (SecondCallCompleted == TRUE);
    }
                                                );
    SecondEnteredBeforeRelease      = SecondProviderEntered;
    SecondCompletedWithoutProvider  = (BOOLEAN)(SecondCallCompleted && !SecondProviderEntered);
    ReleaseProvider                 = TRUE;
  }

  ProviderCondition.notify_all ();
  FirstThread.join ();
  SecondThread.join ();

  EXPECT_EQ (ObservedSecondProgress, TRUE);
  EXPECT_EQ (SecondEnteredBeforeRelease, TRUE);
  EXPECT_EQ (SecondCompletedWithoutProvider, FALSE);
  EXPECT_EQ (ProviderEntryCount, 2U);
  EXPECT_EQ (FirstStatus, EFI_SUCCESS);
  EXPECT_EQ (FirstHookInvoked, TRUE);
  EXPECT_EQ (SecondStatus, EFI_SUCCESS);
  EXPECT_EQ (SecondHookInvoked, TRUE);

  VariableRuntimeHookPostSetVariable (FirstHookInvoked, EFI_DEVICE_ERROR);
  VariableRuntimeHookPostSetVariable (SecondHookInvoked, EFI_ABORTED);

  HookInvoked = 0xA5;
  Status      = VariableRuntimeHookPreSetVariable (
                  WarningName,
                  &VendorGuid,
                  Attributes,
                  sizeof (Data),
                  &Data,
                  &HookInvoked
                  );
  EXPECT_EQ (Status, EFI_WARN_UNKNOWN_GLYPH);
  EXPECT_EQ (HookInvoked, FALSE);
}

int
main (
  int   argc,
  char  *argv[]
  )
{
  InitGoogleTest (&argc, argv);
  return RUN_ALL_TESTS ();
}
