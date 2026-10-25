package jsui

/*
#include "shim.h"
*/
import "C"

import (
	"errors"
	"fmt"
	"os"
	"runtime/cgo"
)

func runtimeOf(h C.uintptr_t) *Runtime { return cgo.Handle(h).Value().(*Runtime) }

//export goJsuiLog
func goJsuiLog(h C.uintptr_t, level C.int, msg *C.char) {
	r := runtimeOf(h)
	if r.OnLog != nil {
		r.OnLog(LogLevel(level), goString(msg))
		return
	}
	fmt.Fprintln(os.Stderr, "[jsui]", goString(msg))
}

// goJsuiJSReady runs on the JS thread (Config.OnJSReady).
//
//export goJsuiJSReady
func goJsuiJSReady(h C.uintptr_t) {
	if f := runtimeOf(h).onJSReady; f != nil {
		f()
	}
}

//export goJsuiMessage
func goJsuiMessage(h C.uintptr_t, name, payload *C.char) {
	if r := runtimeOf(h); r.OnMessage != nil {
		r.OnMessage(goString(name), goString(payload))
	}
}

//export goJsuiState
func goJsuiState(h C.uintptr_t, key, value *C.char) {
	r := runtimeOf(h)
	if r.OnState == nil {
		return
	}
	var v *string
	if value != nil {
		s := C.GoString(value)
		v = &s
	}
	r.OnState(goString(key), v)
}

//export goJsuiCall
func goJsuiCall(h C.uintptr_t, id C.uint64_t, name, args *C.char) {
	r := runtimeOf(h)
	c := Call{ID: uint64(id), Name: goString(name), Args: goString(args)}
	if c.Args == "" {
		c.Args = "null"
	}
	if r.OnCall == nil {
		r.RejectCall(c.ID, "no handler for '"+c.Name+"'")
		return
	}
	r.OnCall(c)
}

//export goJsuiCallCancel
func goJsuiCallCancel(h C.uintptr_t, id C.uint64_t) {
	if r := runtimeOf(h); r.OnCallCancel != nil {
		r.OnCallCancel(uint64(id))
	}
}

//export goJsuiCallUIDone
func goJsuiCallUIDone(h C.uintptr_t, _ C.uint64_t, ok C.int, result *C.char) {
	handle := cgo.Handle(h)
	done := handle.Value().(func(string, error))
	handle.Delete()
	if done == nil {
		return
	}
	if ok != 0 {
		done(goString(result), nil)
	} else {
		done("", errors.New(goString(result)))
	}
}
