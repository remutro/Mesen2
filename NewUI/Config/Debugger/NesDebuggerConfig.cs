﻿using Avalonia;
using Avalonia.Media;
using Mesen.Debugger;
using Mesen.Interop;
using ReactiveUI.Fody.Helpers;
using System.Reactive.Linq;
using System.Reactive;
using Mesen.ViewModels;

namespace Mesen.Config
{
	public class NesDebuggerConfig : ViewModelBase
	{
		[Reactive] public bool BreakOnBrk { get; set; } = false;
		[Reactive] public bool BreakOnUnofficialOpCode { get; set; } = false;
		[Reactive] public bool BreakOnCpuCrash { get; set; } = false;
		
		[Reactive] public bool BreakOnBusConflict { get; set; } = false;
		[Reactive] public bool BreakOnDecayedOamRead { get; set; } = false;
		[Reactive] public bool BreakOnPpu2006ScrollGlitch { get; set; } = false;

		public void ApplyConfig()
		{
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnBrk, BreakOnBrk);
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnUnofficialOpCode, BreakOnBrk);
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnCpuCrash, BreakOnBrk);
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnBusConflict, BreakOnBrk);
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnDecayedOamRead, BreakOnBrk);
			ConfigApi.SetDebuggerFlag(DebuggerFlags.NesBreakOnPpu2006ScrollGlitch, BreakOnBrk);
		}
	}
}