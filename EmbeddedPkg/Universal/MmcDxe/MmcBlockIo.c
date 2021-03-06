/** @file
*
*  Copyright (c) 2011-2014, ARM Limited. All rights reserved.
*  Copyright (c) Microsoft Corporation. All rights reserved.
*
*  This program and the accompanying materials
*  are licensed and made available under the terms and conditions of the BSD License
*  which accompanies this distribution.  The full text of the license may be found at
*  http://opensource.org/licenses/bsd-license.php
*
*  THE PROGRAM IS DISTRIBUTED UNDER THE BSD LICENSE ON AN "AS IS" BASIS,
*  WITHOUT WARRANTIES OR REPRESENTATIONS OF ANY KIND, EITHER EXPRESS OR IMPLIED.
*
**/

#include <Library/BaseMemoryLib.h>
#include <Library/TimerLib.h>

#include "Mmc.h"
#define MAX_RETRY_COUNT                     1000
#define MULTI_BLK_XFER_MAX_BLK_CNT          10000
// Perform Integer division DIVIDEND/DIVISOR and return the result rounded up or down
// to the nearest integer, where 3.5 and 3.75 are near 4, while 3.25 is near 3.
#define INT_DIV_ROUND(DIVIDEND, DIVISOR)    (((DIVIDEND) + ((DIVISOR) / 2)) / (DIVISOR))
#define MMCI0_BLOCKLEN                      512
#define MMCI0_TIMEOUT                       10000

EFI_STATUS
MmcNotifyState (
  IN MMC_HOST_INSTANCE *MmcHostInstance,
  IN MMC_STATE State
  )
{
  MmcHostInstance->State = State;
  return MmcHostInstance->MmcHost->NotifyState (MmcHostInstance->MmcHost, State);
}

EFI_STATUS
EFIAPI
MmcGetCardStatus (
  IN MMC_HOST_INSTANCE     *MmcHostInstance
  )
{
  EFI_STATUS              Status;
  UINT32                  Response[4];
  UINTN                   CmdArg;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;

  Status = EFI_SUCCESS;
  MmcHost = MmcHostInstance->MmcHost;
  CmdArg = 0;

  if (MmcHost == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  if (MmcHostInstance->State != MmcHwInitializationState) {
    //Get the Status of the card.
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MmcGetCardStatus(MMC_CMD13): Error and Status = %r\n", Status));
      return Status;
    }

    //Read Response
    MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
    PrintResponseR1 (Response[0]);
  }

  return Status;
}

EFI_STATUS
EFIAPI
MmcIdentificationMode (
  IN MMC_HOST_INSTANCE     *MmcHostInstance
  )
{
  EFI_STATUS              Status;
  UINT32                  Response[4];
  UINTN                   Timeout;
  UINTN                   CmdArg;
  BOOLEAN                 IsHCS;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;

  MmcHost = MmcHostInstance->MmcHost;
  CmdArg = 0;
  IsHCS = FALSE;

  if (MmcHost == NULL) {
    return EFI_INVALID_PARAMETER;
  }
  
  //Force MSCH into HWinit state before Identified, it really need.
  MmcHostInstance->State = MmcHwInitializationState;

  // We can get into this function if we restart the identification mode
  if (MmcHostInstance->State == MmcHwInitializationState) {
    // Initialize the MMC Host HW
    Status = MmcNotifyState (MmcHostInstance, MmcHwInitializationState);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MmcIdentificationMode() : Error MmcHwInitializationState\n"));
      return Status;
    }
  }

  Status = MmcHost->SendCommand (MmcHost, MMC_CMD0, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode(MMC_CMD0): Error\n"));
    return Status;
  }

  Status = MmcNotifyState (MmcHostInstance, MmcIdleState);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode() : Error MmcIdleState\n"));
    return Status;
  }

  // Are we using SDIO ?
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD5, 0);

#if 1 // Added for Panda Board
  /* It seems few SD cards need some time to recover from this command? */
  MicroSecondDelay(1000);
#endif
  
  if (Status == EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode(MMC_CMD5): Error - SDIO not supported.\n"));
    return EFI_UNSUPPORTED;
  }

  // Check which kind of card we are using. Ver2.00 or later SD Memory Card (PL180 is SD v1.1)
  CmdArg = (0x0UL << 12 | BIT8 | 0xCEUL << 0);
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD8, CmdArg);
  if (Status == EFI_SUCCESS) {
    DEBUG ((EFI_D_ERROR, "Card is SD2.0 => Supports high capacity\n"));
    IsHCS = TRUE;
    MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R7, Response);
    PrintResponseR1 (Response[0]);
    //check if it is valid response
    if (Response[0] != CmdArg) {
      DEBUG ((EFI_D_ERROR, "The Card is not usable\n"));
      return EFI_UNSUPPORTED;
    }
  } else {
    DEBUG ((EFI_D_ERROR, "Not a SD2.0 Card\n"));
  }

  // We need to wait for the MMC or SD card is ready => (gCardInfo.OCRData.PowerUp == 1)
  Timeout = MAX_RETRY_COUNT;
  while (Timeout > 0) {
    // SD Card or MMC Card ? CMD55 indicates to the card that the next command is an application specific command
    Status = MmcHost->SendCommand (MmcHost, MMC_CMD55, 0);
    if (Status == EFI_SUCCESS) {
      DEBUG ((EFI_D_INFO, "Card should be SD\n"));
      if (IsHCS) {
        MmcHostInstance->CardInfo.CardType = SD_CARD_2;
      } else {
        MmcHostInstance->CardInfo.CardType = SD_CARD;
      }

      // Note: The first time CmdArg will be zero
      CmdArg = ((UINTN *) &(MmcHostInstance->CardInfo.OCRData))[0];
      if (IsHCS) {
        CmdArg |= BIT30;
      }
      Status = MmcHost->SendCommand (MmcHost, MMC_ACMD41, CmdArg);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_OCR, Response);
        ((UINT32 *) &(MmcHostInstance->CardInfo.OCRData))[0] = Response[0];
      }
    } else {
      DEBUG ((EFI_D_INFO, "Card should be MMC\n"));
      MmcHostInstance->CardInfo.CardType = MMC_CARD;

      Status = MmcHost->SendCommand (MmcHost, MMC_CMD1, 0x40800000);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_OCR, Response);
        ((UINT32 *) &(MmcHostInstance->CardInfo.OCRData))[0] = Response[0];
      }
    }

    if (!EFI_ERROR (Status)) {
      if (!MmcHostInstance->CardInfo.OCRData.PowerUp) {
        MicroSecondDelay (1);
        Timeout--;
      } else {
        if ((MmcHostInstance->CardInfo.CardType == SD_CARD_2) && (MmcHostInstance->CardInfo.OCRData.AccessMode & BIT1)) {
          MmcHostInstance->CardInfo.CardType = SD_CARD_2_HIGH;
          DEBUG ((EFI_D_ERROR, "High capacity card.\n"));
        }
        break;  // The MMC/SD card is ready. Continue the Identification Mode
      }
    } else {
      MicroSecondDelay (1);
      Timeout--;
    }
  }

  if (Timeout == 0) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode(): No Card\n"));
    return EFI_NO_MEDIA;
  } else {
    PrintOCR (Response[0]);
  }

  Status = MmcNotifyState (MmcHostInstance, MmcReadyState);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode() : Error MmcReadyState\n"));
    return Status;
  }

  Status = MmcHost->SendCommand (MmcHost, MMC_CMD2, 0);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode(MMC_CMD2): Error\n"));
    return Status;
  }
  MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_CID, Response);
  PrintCID ((CID*)Response);

  Status = MmcNotifyState (MmcHostInstance, MmcIdentificationState);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode() : Error MmcIdentificationState\n"));
    return Status;
  }

  //
  // Note, SD specifications say that "if the command execution causes a state change, it
  // will be visible to the host in the response to the next command"
  // The status returned for this CMD3 will be 2 - identification
  //
  CmdArg = 1;
  //CMD3 with RCA as argument, should be lsr 16
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD3, CmdArg << 16);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode(MMC_CMD3): Error\n"));
    return Status;
  }

  MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_RCA, Response);
  PrintRCA (Response[0]);

  // For MMC card, RCA is assigned by CMD3 while CMD3 dumps the RCA for SD card
  if (MmcHostInstance->CardInfo.CardType != MMC_CARD) {
    MmcHostInstance->CardInfo.RCA = Response[0] >> 16;
  } else {
    MmcHostInstance->CardInfo.RCA = CmdArg;
  }

  Status = MmcNotifyState (MmcHostInstance, MmcStandByState);
  if (EFI_ERROR (Status)) {
    DEBUG ((EFI_D_ERROR, "MmcIdentificationMode() : Error MmcStandByState\n"));
    return Status;
  }

  return EFI_SUCCESS;
}

EFI_STATUS InitializeMmcDevice (
  IN  MMC_HOST_INSTANCE   *MmcHostInstance
  )
{
  UINT32                  Response[4];
  EFI_STATUS              Status;
  UINT64                  DeviceSize, NumBlocks, CardSizeBytes, CardSizeGB;
  UINT32                  CmdArg;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;
  UINT32                  BlockCount;
  UINT32                  ECSD[128];

  BlockCount = 1;
  MmcHost = MmcHostInstance->MmcHost;

  MmcIdentificationMode (MmcHostInstance);

  //Send a command to get Card specific data
  CmdArg = MmcHostInstance->CardInfo.RCA << 16;
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD9, CmdArg);
  if (EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "InitializeMmcDevice(MMC_CMD9): Error, Status=%r\n", Status));
    return Status;
  }
  //Read Response
  MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_CSD, Response);

  PrintCSD((CSD*)Response);
  
  if (MmcHostInstance->CardInfo.CardType == SD_CARD_2_HIGH) {
      CSD_2* Csd2 = (CSD_2*)Response;
      DeviceSize = Csd2 ->C_SIZE;
      CardSizeBytes = (UINT64)(DeviceSize + 1) * 512llu * 1024llu;
  } else {
      CSD* Csd = (CSD*)Response;
      DeviceSize = ((Csd->C_SIZEHigh10 << 2) | Csd->C_SIZELow2);
      UINT32 BLOCK_LEN = 1 << Csd->READ_BL_LEN;
      UINT32 MULT = 1 << (Csd->C_SIZE_MULT + 2);
      UINT32 BLOCKNR = (DeviceSize + 1) * MULT;
      CardSizeBytes = BLOCKNR * BLOCK_LEN;
  }

  NumBlocks = (CardSizeBytes / MMCI0_BLOCKLEN);
  CardSizeGB = INT_DIV_ROUND(CardSizeBytes, (UINT64)(1024llu * 1024llu * 1024llu));
  DEBUG((
      EFI_D_INIT,
      "MmcDxe: CardSize: %ldGB, NumBlocks: %ld assuming BlockSize: %d\n",
      (UINT64)CardSizeGB,
      (UINT64)NumBlocks,
      MMCI0_BLOCKLEN));
  
  MmcHostInstance->BlockIo.Media->LastBlock = (NumBlocks - 1);
  MmcHostInstance->BlockIo.Media->BlockSize = MMCI0_BLOCKLEN;
  MmcHostInstance->BlockIo.Media->ReadOnly = MmcHost->IsReadOnly(MmcHost);
  MmcHostInstance->BlockIo.Media->MediaPresent = TRUE;
  MmcHostInstance->BlockIo.Media->MediaId++;

  CmdArg = MmcHostInstance->CardInfo.RCA << 16;
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD7, CmdArg);
  if (EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "InitializeMmcDevice(MMC_CMD7): Error and Status = %r\n", Status));
    return Status;
  }

  Status = MmcNotifyState (MmcHostInstance, MmcTransferState);
  if (EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "InitializeMmcDevice(): Error MmcTransferState\n"));
    return Status;
  }

  if(MmcHostInstance->CardInfo.CardType == MMC_CARD) {
  	 // Fetch ECSD
  	Status = MmcHost->SendCommand (MmcHost, MMC_CMD8, CmdArg);
  	if (EFI_ERROR (Status)) {
    	  DEBUG ((EFI_D_ERROR, "EmmcIdentificationMode(): ECSD fetch error, Status=%r.\n", Status));
  	}

  	Status = MmcHost->ReadBlockData (MmcHost, 0, 512, ECSD);
  	if (EFI_ERROR (Status)) {
    	  DEBUG ((EFI_D_ERROR, "EmmcIdentificationMode(): ECSD read error, Status=%r.\n", Status));
    	  return Status;
  	}
	MmcHostInstance->BlockIo.Media->LastBlock    = ECSD[53] - 1;
	MmcHostInstance->BlockIo.Media->BlockSize    = 512;
  	DEBUG((EFI_D_ERROR, "EmmcIdentificationMode: NumBlocks: %ld, BlockSize: 512\n",
		MmcHostInstance->BlockIo.Media->LastBlock));
	DEBUG((EFI_D_ERROR, "EmmcIdentificationMode: capability = %ld Bytes\n", (MmcHostInstance->BlockIo.Media->LastBlock)<<9));
  }

  // Set Block Length
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD16, MmcHostInstance->BlockIo.Media->BlockSize);
  if (EFI_ERROR (Status)) {
    DEBUG((EFI_D_ERROR, "InitializeMmcDevice(MMC_CMD16): Error MmcHostInstance->BlockIo.Media->BlockSize: %d and Error = %r\n",
                        MmcHostInstance->BlockIo.Media->BlockSize, Status));
    return Status;
  }

  // Block Count (not used). Could return an error for SD card
  if (MmcHostInstance->CardInfo.CardType == MMC_CARD) {
    MmcHost->SendCommand (MmcHost, MMC_CMD23, BlockCount);
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MmcReset (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN BOOLEAN                  ExtendedVerification
  )
{
  MMC_HOST_INSTANCE       *MmcHostInstance;

  MmcHostInstance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);

  if (MmcHostInstance->MmcHost == NULL) {
    // Nothing to do
    return EFI_SUCCESS;
  }

  // If a card is not present then clear all media settings
  if (!MmcHostInstance->MmcHost->IsCardPresent (MmcHostInstance->MmcHost)) {
    MmcHostInstance->BlockIo.Media->MediaPresent = FALSE;
    MmcHostInstance->BlockIo.Media->LastBlock    = 0;
    MmcHostInstance->BlockIo.Media->BlockSize    = 512;  // Should be zero but there is a bug in DiskIo
    MmcHostInstance->BlockIo.Media->ReadOnly     = FALSE;

    // Indicate that the driver requires initialization
    MmcHostInstance->State = MmcHwInitializationState;

    return EFI_SUCCESS;
  }

  // Implement me. Either send a CMD0 (could not work for some MMC host) or just turn off/turn
  //      on power and restart Identification mode
  return EFI_SUCCESS;
}

EFI_STATUS
MmcDetectCard (
  EFI_MMC_HOST_PROTOCOL     *MmcHost
  )
{
  if (!MmcHost->IsCardPresent (MmcHost)) {
    return EFI_NO_MEDIA;
  } else {
    return EFI_SUCCESS;
  }
}

EFI_STATUS
MmcStopTransmission (
  EFI_MMC_HOST_PROTOCOL     *MmcHost
  )
{
  EFI_STATUS              Status;
  UINT32                  Response[4];
  // Command 12 - Stop transmission (ends read or write)
  // Normally only needed for streaming transfers or after error.
  Status = MmcHost->SendCommand (MmcHost, MMC_CMD12, 0);
  if (!EFI_ERROR (Status)) {
    MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1b, Response);
  }
  return Status;
}

EFI_STATUS
MmcIoBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINTN                    Transfer,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  OUT VOID                    *Buffer
  )
{
  UINT32                  Response[4];
  EFI_STATUS              Status;
  UINTN                   CmdArg;
  INTN                    Timeout;
  UINTN                   Cmd;
  MMC_HOST_INSTANCE       *MmcHostInstance;
  EFI_MMC_HOST_PROTOCOL   *MmcHost;
  UINTN                   BytesRemainingToBeTransfered;
  UINTN                   BlockCount;
  UINTN                   CurrentBlockNum;

  BlockCount = 1;
  MmcHostInstance = MMC_HOST_INSTANCE_FROM_BLOCK_IO_THIS (This);
  ASSERT (MmcHostInstance != NULL);
  MmcHost = MmcHostInstance->MmcHost;
  ASSERT (MmcHost);

  if (This->Media->MediaId != MediaId) {
    return EFI_MEDIA_CHANGED;
  }

  if ((MmcHost == NULL) || (Buffer == NULL)) {
    return EFI_INVALID_PARAMETER;
  }

  // Check if a Card is Present
  if (!MmcHostInstance->BlockIo.Media->MediaPresent) {
    return EFI_NO_MEDIA;
  }

  // All blocks must be within the device
  if ((Lba + (BufferSize / This->Media->BlockSize)) > (This->Media->LastBlock + 1)) {
    return EFI_INVALID_PARAMETER;
  }

  if ((Transfer == MMC_IOBLOCKS_WRITE) && (This->Media->ReadOnly == TRUE)) {
    return EFI_WRITE_PROTECTED;
  }

  // Reading 0 Byte is valid
  if (BufferSize == 0) {
    return EFI_SUCCESS;
  }

  // The buffer size must be an exact multiple of the block size
  if ((BufferSize % This->Media->BlockSize) != 0) {
    return EFI_BAD_BUFFER_SIZE;
  }

  // Check the alignment
  if ((This->Media->IoAlign > 2) && (((UINTN)Buffer & (This->Media->IoAlign - 1)) != 0)) {
    return EFI_INVALID_PARAMETER;
  }

  BytesRemainingToBeTransfered = BufferSize;
  while (BytesRemainingToBeTransfered > 0) {

    // Check if the Card is in Ready status
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Response[0] = 0;
    Timeout = 20;
    while(   (!(Response[0] & MMC_R0_READY_FOR_DATA))
          && (MMC_R0_CURRENTSTATE (Response) != MMC_R0_STATE_TRAN)
          && Timeout--) {
      Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
      }
    }

    if (0 == Timeout) {
      DEBUG ((EFI_D_ERROR, "The Card is busy\n"));
      return EFI_NOT_READY;
    }

    //Set command argument based on the card access mode (Byte mode or Block mode)
    if (MmcHostInstance->CardInfo.OCRData.AccessMode & BIT1) {
      CmdArg = Lba;
    } else {
      CmdArg = Lba * This->Media->BlockSize;
    }

    if (Transfer == MMC_IOBLOCKS_READ) {
      BlockCount = BytesRemainingToBeTransfered / This->Media->BlockSize;

      // The card is unhappy if trying to read too many blocks at once
      if (BlockCount > MULTI_BLK_XFER_MAX_BLK_CNT) {
        BlockCount = MULTI_BLK_XFER_MAX_BLK_CNT;
      }
      if (BlockCount == 1) {
        // Read a single block
        Cmd = MMC_CMD17;
      }
      else {
        // Read multiple blocks
        Cmd = MMC_CMD18;

        // Prepare for multi-block transfer by first sending the BlockCount
        Status = MmcHost->SendCommand(MmcHost, MMC_CMD23, BlockCount);
        if (EFI_ERROR (Status)) {
          DEBUG ((EFI_D_ERROR, "MmcIoBlocks(MMC_CMD23, Sending BlockCount for Multiple Block Read): Error %r\n",
            Status));
          return Status;
        }
      }
    } else {
      // Write a single block
      Cmd = MMC_CMD24;
    }
    Status = MmcHost->SendCommand (MmcHost, Cmd, CmdArg);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MmcIoBlocks(MMC_CMD%d): Error %r\n", Cmd, Status));
      return Status;
    }

    if (Transfer == MMC_IOBLOCKS_READ) {
      if (BlockCount == 1) {
        // Read one block of Data
        Status = MmcHost->ReadBlockData (MmcHost, Lba, This->Media->BlockSize, Buffer);
      }
      else {
        // Read multiple blocks of Data
        for (CurrentBlockNum = 0; CurrentBlockNum < BlockCount; CurrentBlockNum++) {
          Status = MmcHost->ReadBlockData(MmcHost, Lba, This->Media->BlockSize, Buffer);
          if (EFI_ERROR (Status)) {
            DEBUG ((EFI_D_ERROR, "MmcIoBlocks(): Error Read Multiple Block Data and Status = %r\n", Status));
            MmcStopTransmission (MmcHost);
            return Status;
          }
          BytesRemainingToBeTransfered -= This->Media->BlockSize;
          Lba += 1;
          Buffer = (UINT8 *)Buffer + This->Media->BlockSize;
        }
        MmcStopTransmission (MmcHost);
      }

      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "MmcIoBlocks(): Error Read Block Data and Status = %r\n", Status));
        MmcStopTransmission (MmcHost);
        return Status;
      }
      Status = MmcNotifyState (MmcHostInstance, MmcProgrammingState);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "MmcIoBlocks() : Error MmcProgrammingState\n"));
        return Status;
      }
    } else {
      // Write one block of Data
      Status = MmcHost->WriteBlockData (MmcHost, Lba, This->Media->BlockSize, Buffer);
      if (EFI_ERROR (Status)) {
        DEBUG ((EFI_D_ERROR, "MmcIoBlocks(): Error Write Block Data and Status = %r\n", Status));
        MmcStopTransmission (MmcHost);
        return Status;
      }
    }

    // Command 13 - Read status and wait for programming to complete (return to tran)
    Timeout = MMCI0_TIMEOUT;
    CmdArg = MmcHostInstance->CardInfo.RCA << 16;
    Response[0] = 0;
    while(   (!(Response[0] & MMC_R0_READY_FOR_DATA))
          && (MMC_R0_CURRENTSTATE (Response) != MMC_R0_STATE_TRAN)
          && Timeout--) {
      Status = MmcHost->SendCommand (MmcHost, MMC_CMD13, CmdArg);
      if (!EFI_ERROR (Status)) {
        MmcHost->ReceiveResponse (MmcHost, MMC_RESPONSE_TYPE_R1, Response);
        if ((Response[0] & MMC_R0_READY_FOR_DATA)) {
          break;  // Prevents delay once finished
        }
      }
      NanoSecondDelay (100);
    }

    Status = MmcNotifyState (MmcHostInstance, MmcTransferState);
    if (EFI_ERROR (Status)) {
      DEBUG ((EFI_D_ERROR, "MmcIoBlocks() : Error MmcTransferState\n"));
      return Status;
    }

    // Bookkeeping for multiple block transfer was already performed above
    if (BlockCount == 1) {
      BytesRemainingToBeTransfered -= This->Media->BlockSize;
      Lba += BlockCount;
      Buffer = (UINT8 *)Buffer + This->Media->BlockSize;
    }
  }

  return EFI_SUCCESS;
}

EFI_STATUS
EFIAPI
MmcReadBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  OUT VOID                    *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_READ, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcWriteBlocks (
  IN EFI_BLOCK_IO_PROTOCOL    *This,
  IN UINT32                   MediaId,
  IN EFI_LBA                  Lba,
  IN UINTN                    BufferSize,
  IN VOID                     *Buffer
  )
{
  return MmcIoBlocks (This, MMC_IOBLOCKS_WRITE, MediaId, Lba, BufferSize, Buffer);
}

EFI_STATUS
EFIAPI
MmcFlushBlocks (
  IN EFI_BLOCK_IO_PROTOCOL  *This
  )
{
  return EFI_SUCCESS;
}
