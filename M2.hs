module M2
  ( module M1 aliases
-- exports due to IEModuleContents NoExt mod:M1 Nothing: [][]
  )
where
import M1 aliases_hiding ()
a = L.intercalate
-- b :: Ord k0 => (a -> Maybe a) -> k0 -> L.Map k0 a -> L.Map k0 a
-- b = L.update

