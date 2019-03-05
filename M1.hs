module M1
  (
    module L aliases (L)
-- exports due to IEModuleContents NoExt mod:L (Just mod:L): [][(mod:L,av:name:occV:intercalate),(mod:L,av:name:occV:intersperse)]
  )
where

import Data.List as L (intercalate, intersperse)
import Data.List (intercalate, intersperse)
-- import Data.Map  as L hiding (map, filter, null, foldr, foldl', foldl, splitAt, insert, partition, union, drop, lookup, delete, findIndex, (\\), take)
-- import Data.Map hiding (map, filter, null, foldr, foldl', foldl, splitAt, insert, partition, union, drop, lookup, delete, findIndex, (\\), take)
