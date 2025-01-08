<script lang="ts">
  import GeneralConfigEditor from '$lib/components/editors/general-config-editor.svelte';
  import { onMount } from 'svelte';
  import { configStore } from '$lib/stores/config';

  onMount(async () => {
    await configStore.load();
  });
</script>

<GeneralConfigEditor /> 