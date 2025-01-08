<script lang="ts">
  import { DropdownMenu as DropdownMenuPrimitive } from "bits-ui";
  import Item from "./dropdown-menu-item.svelte";
  import Label from "./dropdown-menu-label.svelte";
  import Content from "./dropdown-menu-content.svelte";
  import Shortcut from "./dropdown-menu-shortcut.svelte";
  import Separator from "./dropdown-menu-separator.svelte";
  import RadioGroup from "./dropdown-menu-radio-group.svelte";
  import RadioItem from "./dropdown-menu-radio-item.svelte";
  import SubContent from "./dropdown-menu-sub-content.svelte";
  import SubTrigger from "./dropdown-menu-sub-trigger.svelte";
  import CheckboxItem from "./dropdown-menu-checkbox-item.svelte";

  export let sideEffects = true;
</script>

<DropdownMenuPrimitive.Root {sideEffects} {...$$restProps}>
  <slot />
</DropdownMenuPrimitive.Root> 