// Copyright 2023 The Forgotten Server Authors. All rights reserved.
// Use of this source code is governed by the GPL-2.0 License that can be found in the LICENSE file.

#include "otpch.h"
#include "outputmessage.h"

#include <boost/lockfree/stack.hpp>

static boost::lockfree::stack<OutputMessage*, boost::lockfree::capacity<2048>> g_outputStack;

OutputMessage_ptr OutputMessage::make(void){
	OutputMessage *output;
	if(!g_outputStack.pop(output)){
		output = new OutputMessage;
	}
	return OutputMessage_ptr(output);
}

void OutputMessageDeleter::operator()(OutputMessage *msg) const {
	if(!g_outputStack.bounded_push(msg)){
		delete msg;
	}
}

