(* Copyright (C) 2009,2019,2022 Matthew Fluet.
 * Copyright (C) 2004-2005 Henry Cejtin, Matthew Fluet, Suresh
 *    Jagannathan, and Stephen Weeks.
 *
 * MLton is released under a HPND-style license.
 * See the file MLton-LICENSE for details.
 *)

structure MLton =
   struct
      val isMLton = false
      structure Exn =
         struct
            val history = SMLofNJ.exnHistory
         end
      structure GC =
         struct
            fun collect () = SMLofNJ.Internals.GC.doGC 8
            fun pack () = collect ()
         end
      structure Process = MLtonProcess
   end
