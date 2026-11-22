#pragma once

// MIT adapter header enabling untouched MIT Microsoft Xbox-GDK-Samples
// SimpleDirectStorageCombo/SampleImplementations/*.cpp to link directly against dstorage_xs.dll.

#include <dstorage_xs.h>

using IDStorageFile = IDStorageFileX;
using IDStorageFactory = IDStorageFactoryX1;
using IDStorageStatusArray = IDStorageStatusArrayX;
using IDStorageQueue = IDStorageQueueX;
using IDStorageQueue1 = IDStorageQueueX1;
