<script lang="ts">
  import { Button } from "$lib/components/ui/button";
  import { Card, CardContent, CardHeader, CardTitle } from "$lib/components/ui/card";
  import GeneralConfigEditor from "$lib/components/editors/general-config-editor.svelte";
</script>

<div class="container mx-auto p-6">
  <h1 class="text-2xl font-bold mb-6">General Configuration</h1>
  <GeneralConfigEditor />
</div> 